#include <event2/util.h>
#include <inttypes.h>
#include <string.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "tmux.h"

typedef void (*remote_request_cb)(struct remote *, char *, enum cmd_retval);
struct remote_request {
	const char	 *command;
	remote_request_cb callback;

	TAILQ_ENTRY(remote_request) entry;
};

struct remote {
	struct session	   *s;
	struct bufferevent *event;
	struct bufferevent *pane0_event;
	struct evbuffer	   *line_buffer;

	int		 in_block;
	struct evbuffer *block_buffer;
	long		 block_time;
	u_int		 block_number;

	TAILQ_HEAD(, remote_request) requests;
};

static void remote_read_callback(struct bufferevent *, void *);
static void remote_read_line(struct remote *, struct evbuffer *);
static void remote_error(struct remote *, char *);

static void remote_begin_block(struct remote *, char *);
static void remote_dispatch_reply(struct remote *, char *, enum cmd_retval);
static void remote_dispatch_event(struct remote *, char *);
static void remote_output(struct remote *, char *);
static void remote_session_changed(struct remote *, char *);

static void remote_init_dumb(struct remote *);

struct remote *
remote_create(struct bufferevent *bev)
{
	struct remote  *r;
	struct environ *env;
	struct options *oo;

	r = xcalloc(1, sizeof *r);
	r->line_buffer = evbuffer_new();
	r->block_buffer = evbuffer_new();
	r->event = bev;
	TAILQ_INIT(&r->requests);

	bufferevent_setcb(bev, remote_read_callback, NULL, NULL, r);

	/* XXX: request information about session */

	env = environ_create();
	oo = options_create(global_s_options);
	r->s = session_create("remote", "localhost", "/tmp", env, oo, NULL);
	remote_init_dumb(r);

	return (r);
}

/* Move line from an evbuffer into another evbuffer, draining
   the bytes from the source buffer. */
static int
evbuffer_remove_line(struct evbuffer *src, struct evbuffer *dst)
{
	char		    crlf[2] = {0};
	struct evbuffer_ptr eol;

	eol = evbuffer_search_eol(src, NULL, NULL, EVBUFFER_EOL_ANY);
	if (eol.pos < 0)
		return (0);

	evbuffer_remove_buffer(src, dst, eol.pos);
	evbuffer_add(dst, "", 1); /* add \0 terminator */

	evbuffer_copyout(src, crlf, 2);
	if (memcmp(crlf, "\r\n", 2) == 0 || memcmp(crlf, "\n\r", 2) == 0)
		evbuffer_drain(src, 2);
	else
		evbuffer_drain(src, 1);

	return (1);
}

static void
remote_read_callback(struct bufferevent *bev, void *data)
{
	struct remote	*r = data;
	struct evbuffer *in = bufferevent_get_input(bev);
	struct evbuffer *line = r->line_buffer;

	/* read stream line-by-line */
	while (evbuffer_remove_line(in, line)) {
		remote_read_line(r, line);
		evbuffer_drain(line, EV_SIZE_MAX);
	}

	/* add incomplete bytes, resetting high watermark */
	bufferevent_read_buffer(bev, line);
}

static void
remote_read_line(struct remote *r, struct evbuffer *buf)
{
	char  prefix[7] = {0};
	char *line;

	evbuffer_copyout(buf, prefix, sizeof prefix);

	if (r->in_block) {
		if (strncmp(prefix, "%end ", 5) == 0) {
			line = evbuffer_pullup(buf, -1);
			remote_dispatch_reply(r, line, CMD_RETURN_NORMAL);
			r->in_block = 0;
		} else if (strncmp(prefix, "%error ", 7) == 0) {
			line = evbuffer_pullup(buf, -1);
			remote_dispatch_reply(r, line, CMD_RETURN_ERROR);
			r->in_block = 0;
		} else {
			evbuffer_remove_buffer(buf, r->block_buffer, EV_SIZE_MAX);
		}
	} else {
		line = evbuffer_pullup(buf, -1);
		if (strncmp(prefix, "%begin ", 7) == 0) {
			remote_begin_block(r, line);
			r->in_block = 1;
		} else {
			remote_dispatch_event(r, line);
		}
	}
}

static void
remote_begin_block(struct remote *r, char *header)
{
	int flags;

	if (sscanf(header, "%%begin %ld %u %u", &r->block_time,
		    &r->block_number, &flags) != 3)
		fatalx("bad protocol: %s", header);
}

static void
remote_dispatch_reply(struct remote *r, char *footer, enum cmd_retval status)
{
	struct remote_request *req;
	long		       time;
	u_int		       number, flags;

	if (sscanf(footer, "%*s %ld %u %u", &time, &number, &flags) != 3)
		fatalx("bad protocol: %s", footer);

	if (r->block_time != time || r->block_number != number)
		fatalx("footer does not match header: %s", footer);

	if (flags & 1) /* client-originated command */ {
		req = TAILQ_FIRST(&r->requests);
		if (req == NULL)
			fatalx("no matching requests");

		req->callback(r, evbuffer_pullup(r->block_buffer, -1), status);

		TAILQ_REMOVE(&r->requests, req, entry);
		free(req);
	}

	evbuffer_drain(r->block_buffer, EV_SIZE_MAX);
}

static void
remote_dispatch_event(struct remote *r, char *line)
{
	if (strncmp(line, "%output ", 8) == 0) {
		remote_output(r, line);
	} else if (strncmp(line, "%session-changed ", 17) == 0) {
		remote_session_changed(r, line);
	}
}

static void
remote_output(struct remote *r, char *event)
{
	u_int pane_id;
	int mark = -1;
	int off = 0;
	int i;

	if (sscanf(event, "%%output ""%%%u%*1[ ]%n", &pane_id, &mark) != 1)
		fatalx("bad protocol: %s", event);
	if (mark < 0)
		fatalx("bad protocol: %s", event);

	for (i = mark; event[i]; ++i) {
		char c = event[i];

		if (c < ' ' || c != '\\')
			goto next;

		// Read exactly three bytes of octal values, or else set c to '?'.
		c = 0;
		for (int j = 0; j < 3; j++) {
			i++; --off;
			if (event[i] == '\r') {
				/* Ignore \r's that the line driver sprinkles in at its pleasure. */
				continue;
			}
			if (event[i] < '0' || event[i] > '7') {
				c = '?';
				i--; ++off;  // Back up in case bytes[i] is a null; we don't want to go off the end.
				break;
			}
			c *= 8;
			c += event[i] - '0';
		}

	next:
		event[i+off] = c;
	}

	bufferevent_write(r->pane0_event, event + mark, i - mark + off);
	bufferevent_flush(r->pane0_event, EV_WRITE, BEV_FLUSH);
}

static void
remote_session_changed(struct remote *r, char *event)
{
	u_int new_id;
	int mark = -1;

	if (sscanf(event, "%%session-changed $%u %n", &new_id, &mark) != 1)
		fatalx("bad protocol");

	if (mark < 0)
		fatalx("bad protocol");
}

static void
remote_init_dumb(struct remote *r)
{
	struct bufferevent  *pipe[2];
	struct window	    *w;
	struct window_pane  *new_wp;
	struct spawn_context sc = {0};
	char		    *cause = NULL;
	u_int		     sx, sy, xpixel, ypixel;
	u_int		     hlimit;

	if ((sc.wl = winlink_add(&r->s->windows, sc.idx)) == NULL) {
		//xasprintf(cause, "couldn't add window %d", sc.idx);
		return;
	}

	default_window_size(sc.tc, r->s, NULL, &sx, &sy, &xpixel, &ypixel, -1);
	if ((w = window_create(sx, sy, xpixel, ypixel)) == NULL) {
		winlink_remove(&r->s->windows, sc.wl);
		//xasprintf(cause, "couldn't create window %d", idx);
		return;
	}
	if (r->s->curw == NULL)
		r->s->curw = sc.wl;
	sc.wl->session = r->s;
	w->latest = sc.tc;
	winlink_set_window(sc.wl, w);

	hlimit = options_get_number(r->s->options, "history-limit");
	new_wp = window_add_pane(w, NULL, hlimit, 0);

	bufferevent_pair_new(NULL, 0, pipe);
	r->pane0_event = pipe[0];

	window_pane_set_event_nofd(new_wp, pipe[1]);
	window_set_active_pane(w, new_wp, 0);
	layout_init(w, new_wp);
}

void
remote_destroy(struct remote *r)
{
	evbuffer_free(r->line_buffer);
	evbuffer_free(r->block_buffer);
	bufferevent_free(r->event);
	session_destroy(r->s, 1, __func__);
}
