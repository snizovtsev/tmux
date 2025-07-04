#include <event2/util.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>>

#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "tmux.h"

struct remote_query;
typedef void (*remote_query_cb)(struct remote *, struct remote_query *);

struct remote_query {
	const char     *command;
	remote_query_cb done;
	remote_query_cb error;
	int		arity;

	TAILQ_ENTRY(remote_query) entry;
};

struct remote {
	struct window_pane *wp;
	struct bufferevent *event;

	struct session	     *session;
	struct client_windows windows;
	struct client_windows panes;

	struct evbuffer *line_buffer;
	struct evbuffer *reply_buffer;
	long		 reply_time;
	u_int		 reply_number;

	/* XXX: n-arity request (multiple bodies) */
	TAILQ_HEAD(remote_quries, remote_query) queries;
};

static void remote_read_callback(struct bufferevent *, void *);
static void remote_read_line(struct remote *, struct evbuffer *);

static void remote_begin_reply(struct remote *, u_char *);
static void remote_dispatch_reply(struct remote *, u_char *, int);
static void remote_dispatch_event(struct remote *, u_char *, size_t len);
static void remote_output(struct remote *, u_int, char *);
static void remote_extended_output(struct remote *, u_int, uint64_t, char *);
static void remote_pane_mode_changed(struct remote *, u_int);
static void remote_session_changed(struct remote *, u_int, char *);
static void remote_window_renamed(struct remote *, u_int, char *);
static void remote_unlinked_window_renamed(struct remote *, u_int, char *);
static void remote_session_renamed(struct remote *, u_int, char *);
static void remote_client_session_changed(struct remote *, char *, u_int, char *);
static void remote_window_pane_changed(struct remote *, u_int, u_int);
static void remote_window_close(struct remote *, u_int);
static void remote_unlinked_window_close(struct remote *, u_int);
static void remote_window_add(struct remote *, u_int);
static void remote_unlinked_window_add(struct remote *, u_int);
static void remote_session_window_changed(struct remote *, u_int, u_int);
static void remote_sessions_changed(struct remote *);
static void remote_exit(struct remote *);

static void remote_init_dumb(struct remote *);

static void printflike(2, 0)
remote_log(struct remote *r, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	window_copy_vadd(r->wp, 0, fmt, ap);
	va_end(ap);
}

struct remote *
remote_create(struct bufferevent *bev, struct window_pane *wp)
{
	struct remote  *r;

	r = xcalloc(1, sizeof *r);
	r->line_buffer = evbuffer_new();
	r->reply_buffer = evbuffer_new();
	r->wp = wp;
	r->event = bev;
	TAILQ_INIT(&r->queries);

	bufferevent_setcb(bev, remote_read_callback, NULL, NULL, r);

	remote_log(r, "** enter tmux control mode **");

	return (r);
}

static struct remote_query* printflike(3, 0)
remote_run(struct remote *r, struct remote_query* q, const char *fmt, ...)
{
	va_list ap;

	log_debug("%s: %s", __func__, q->command);

	if (TAILQ_LAST(&r->queries, remote_quries) != q)
		TAILQ_INSERT_TAIL(&r->queries, q, entry);
	q->arity++;

	va_start(ap, fmt);
	evbuffer_add_vprintf(r->event->output, fmt, ap);
	va_end(ap);

	return q;
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

	evbuffer_copyout_from(src, &eol, crlf, 2);
	evbuffer_remove_buffer(src, dst, eol.pos + 1);

	if (memcmp(crlf, "\r\n", 2) == 0 || memcmp(crlf, "\n\r", 2) == 0)
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
remote_read_line(struct remote *r, struct evbuffer *buffer)
{
	u_char *line = evbuffer_pullup(buffer, -1);
	size_t	len = evbuffer_get_length(buffer);

	if (r->reply_number) {
		if (strncmp(line, "%end ", 5) == 0) {
			remote_dispatch_reply(r, line, 0);
			r->reply_number = 0;
		} else if (strncmp(line, "%error ", 7) == 0) {
			remote_dispatch_reply(r, line, 1);
			r->reply_number = 0;
		} else {
			evbuffer_add_buffer(r->reply_buffer, buffer);
		}
	} else {
		if (strncmp(line, "%begin ", 7) == 0)
			remote_begin_reply(r, line);
		else if (line[0] == '%')
			remote_dispatch_event(r, line, len - 1);
		else
			remote_log(r, "%s: protocol error", __func__);
	}
}

static void
remote_begin_reply(struct remote *r, u_char *line)
{
	long	time;
	u_int	number, flags;

	if (sscanf(line, "%%begin%*1[ ]%ld%*1[ ]%u%*1[ ]%u", &time, &number, &flags) != 3) {
		remote_log(r, "%s: protocol error: bad arguments", __func__);
		return;
	}

	r->reply_number = number;
	r->reply_time = time;
}

static void
remote_dispatch_reply(struct remote *r, u_char *footer, int error)
{
	struct remote_query *q;
	remote_query_cb	     complete;
	long		     time;
	u_int		     number, flags;

	if (sscanf(footer, "%*s%*1[ ]%ld%*1[ ]%u%*1[ ]%u", &time, &number, &flags) != 3) {
		remote_log(r, "%s: protocol error: bad arguments", __func__);
		return;
	}

	if (r->reply_time != time || r->reply_number != number) {
		remote_log(r, "%s: protocol error: reply metadata mismatch", __func__);
		return;
	}

	if (flags & 1) /* client-originated command */ {
		q = TAILQ_FIRST(&r->queries);
		if (q == NULL) {
			remote_log(r, "error: no requests pending");
			return;
		}

		complete = error ? q->error : q->done;
		complete(r, q);

		if (--q->arity == 0) {
			TAILQ_REMOVE(&r->queries, q, entry);
			free(q);
		}
	}

	evbuffer_drain(r->reply_buffer, EV_SIZE_MAX);
}

struct try_scanf_ctx {
	const char *line;
	size_t	    len;
	int	    mark;
	char	   *rest;
};

static int scanflike(3, 0)
try_sscanf(struct try_scanf_ctx *ctx, int args, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsscanf(ctx->line, fmt, ap);
	va_end(ap);

	if (ctx->mark > 0) {
		ctx->rest =
		    xstrndup(ctx->line + ctx->mark, ctx->len - ctx->mark);
		++n;
	}

	if (n == args)
		return (1);
	return (0);
}

static void
remote_dispatch_event(struct remote *r, u_char *line, size_t len)
{
	struct try_scanf_ctx ss = {
	    .line = line,
	    .len = len,
	    .mark = -1,
	    .rest = NULL,
	};
	char		  *client = NULL;
	u_int		   session;
	u_int		   pane;
	u_int		   window;
	unsigned long long age;

	/* %output %pane data */
	if (try_sscanf(&ss, 2, "%%output%*1[ ]%%%u%*1[ ]%n", &pane, &ss.mark))
		remote_output(r, pane, ss.rest);
	/* %extended-output %pane age : data */
	else if (try_sscanf(&ss, 3,
		     "%%extended-output%*1[ ]%%%u%*1[ ]%llu%*1[ ]:%*1[ ]%n",
		     &pane, &age, &ss.mark))
		remote_extended_output(r, pane, age, ss.rest);
	/* %session-changed $session session-name */
	else if (try_sscanf(&ss, 2, "%%session-changed%*1[ ]$%u%*1[ ]%n",
		     &session, &ss.mark))
		remote_session_changed(r, session, ss.rest);
	/* %pane-mode-changed %pane */
	else if (try_sscanf(&ss, 1, "%%pane-mode-changed%*1[ ]%%%u", &pane))
		remote_pane_mode_changed(r, pane);
	/* %window-renamed @window new-name */
	else if (try_sscanf(&ss, 2, "%%window-renamed%*1[ ]@%u%*1[ ]%n",
		     &window, &ss.mark))
		remote_window_renamed(r, window, ss.rest);
	/* %unlinked-window-renamed @window new-name */
	else if (try_sscanf(&ss, 2,
		     "%%unlinked-window-renamed%*1[ ]@%u%*1[ ]%n", &window,
		     &ss.mark))
		remote_unlinked_window_renamed(r, window, ss.rest);
	/* %session-renamed $session new-nam */
	else if (try_sscanf(&ss, 2, "%%session-renamed%*1[ ]$%u%*1[ ]%n",
		     &session, &ss.mark))
		remote_session_renamed(r, session, ss.rest);
	/* %client-session-changed pty $session session-name */
	else if (try_sscanf(&ss, 3,
		     "%%client-session-changed%*1[ ]%63ms $%u%*1[ ]%n", &client,
		     &session, &ss.mark))
		remote_client_session_changed(r, client, session, ss.rest);
	/* %window-pane-changed @window %pane */
	else if (try_sscanf(&ss, 2, "%%window-pane-changed%*1[ ]@%u%*1[ ]%%%u",
		     &window, &pane))
		remote_window_pane_changed(r, window, pane);
	/* %window-close @window */
	else if (try_sscanf(&ss, 1, "%%window-close%*1[ ]@%u", &window))
		remote_window_close(r, window);
	/* %unlinked-window-close @window */
	else if (try_sscanf(
		     &ss, 1, "%%unlinked-window-close%*1[ ]@%u", &window))
		remote_unlinked_window_close(r, window);
	/* %window-add @window */
	else if (try_sscanf(&ss, 1, "%%window-add%*1[ ]@%u", &window))
		remote_window_add(r, window);
	/* %unlinked-window-add @window */
	else if (try_sscanf(&ss, 1, "%%unlinked-window-add%*1[ ]@%u", &window))
		remote_unlinked_window_add(r, window);
	/* %session-window-changed $session @window */
	else if (try_sscanf(&ss, 2,
		     "%%session-window-changed%*1[ ]$%u%*1[ ]@%u", &session,
		     &window))
		remote_session_window_changed(r, session, window);
	/* %sessions-changed */
	else if (strncmp(line, "%sessions-changed", 17) == 0)
		remote_sessions_changed(r);
	else if (strncmp(line, "%exit", 5) == 0)
		remote_exit(r);

	free(ss.rest);
	free(client);
}

struct client_pane {
	struct client_window cw;
	struct bufferevent  *event;
};

static void
remote_output(struct remote *r, u_int pane_id, char *data)
{
	struct client_window rb_key = { .window = pane_id };
	struct client_pane  *cp = NULL;

	size_t i;
	int off = 0;

	cp = (struct client_pane*) RB_FIND(client_windows, &r->panes, &rb_key);
	if (cp == NULL) {
		/* TODO: warn */
		return;
	}

	for (i = 0; data[i]; ++i) {
		char c = data[i];

		if (c < ' ' || c != '\\')
			goto next;

		/* Read exactly three bytes of octal values, or else set c to '?'. */
		c = 0;
		for (int j = 0; j < 3; j++) {
			i++; --off;
			if (data[i] == '\r') {
				/* Ignore \r's that the line driver sprinkles in at its pleasure. */
				continue;
			}
			if (data[i] < '0' || data[i] > '7') {
				c = '?';
				i--; ++off;  // Back up in case bytes[i] is a null; we don't want to go off the end.
				break;
			}
			c *= 8;
			c += data[i] - '0';
		}

	next:
		data[i+off] = c;
	}

	bufferevent_write(cp->event, data, i + off);
	bufferevent_flush(cp->event, EV_WRITE, BEV_FLUSH);
}

struct remote_bootstrap_ctx {
	struct remote_query    q;
	struct environ	      *env;
	int		       state;
	const char	      *session_name;
	struct session	      *session;
	struct client_windows  windows;
	struct client_windows  panes;
};

static void
remote_show_environment(struct remote *r, struct environ *env, int flags)
{
	struct evbuffer *output = r->reply_buffer;
	char		*line;

	while ((line = evbuffer_readln(output, NULL, EVBUFFER_EOL_ANY))) {
		if (line[0] != '-') {
			environ_put(env, line, flags);
		} else {
			if (flags)
				environ_set(env, line + 1, flags, "");
			environ_clear(env, line + 1);
		}
		free(line);
	}
}

static void
remote_add_panes(struct remote *r, struct remote_bootstrap_ctx *ctx)
{
	struct session	     *s = ctx->session;
	struct evbuffer	     *output = r->reply_buffer;
	struct window	     *w;
	struct winlink	     *wl;
	struct client_window  find;
	struct client_window *cw;
	struct client_pane   *cp;
	struct window_pane   *wp;
	struct bufferevent   *pipe[2];
	char		     *line, *iter;
	u_int		      window_id, pane_id, window_index, sx, sy;

	u_int hlimit = options_get_number(s->options, "history-limit");

	while ((line = evbuffer_readln(output, NULL, EVBUFFER_EOL_ANY))) {
		remote_log(r, "pane: %s", line);
		iter = line;

		window_id = atol(strsep(&iter, "\t") + /*@*/1);
		window_index = atol(strsep(&iter, "\t"));
		sx = atol(strsep(&iter, "\t"));
		sy = atol(strsep(&iter, "\t"));
		pane_id = atol(strsep(&iter, "\t") + /*%*/1);

		find.window = window_id;
		cw = RB_FIND(client_windows, &ctx->windows, &find);

		if (cw == NULL) {
			wl = winlink_add(&s->windows, window_index);
			wl->session = s;

			w = window_create(sx, sy, 0, 0);
			winlink_set_window(wl, w);
			if (s->curw == NULL)
				s->curw = wl;
		} else {
			w = cw->pane->window;
		}

		wp = window_add_pane(w, NULL, hlimit, 0);
		if (cw == NULL)
			layout_init(w, wp);

		if (cw == NULL) {
			cw = xcalloc(1, sizeof *cw);
			cw->window = window_id;
			cw->pane = wp;
			RB_INSERT(client_windows, &ctx->windows, cw);
		}

		// HACK
		window_set_active_pane(w, wp, 0);

		bufferevent_pair_new(NULL, 0, pipe);
		window_pane_set_event_nofd(wp, pipe[1]);

		cp = xcalloc(1, sizeof *cp);
		cp->cw.window = pane_id;
		cp->cw.pane = wp;
		cp->event = pipe[0];

		RB_INSERT(client_windows, &ctx->panes, &cp->cw);

		free(line);
	}
}

static void
remote_fix_windows(struct remote *r, struct remote_bootstrap_ctx *ctx)
{
	struct evbuffer	     *output = r->reply_buffer;
	struct window	     *w;
	struct client_window *cw;
	struct client_window  find;
	u_int		      id, active;
	char		     *line, *iter, *name, *layout, *flags;
	char		     *cause = NULL;

	while ((line = evbuffer_readln(output, NULL, EVBUFFER_EOL_ANY))) {
		remote_log(r, "window: %s", line);
		iter = line;

		id = atol(strsep(&iter, "\t") + /*@*/1);
		name = strsep(&iter, "\t");
		layout = strsep(&iter, "\t");
		flags = strsep(&iter, "\t");
		active = atoi(strsep(&iter, "\t"));

		find.window = id;
		cw = RB_FIND(client_windows, &ctx->windows, &find);
		w = cw->pane->window;

		if (active)
			ctx->session->curw = TAILQ_FIRST(&w->winlinks);

		window_set_name(w, name);
		layout_parse(w, layout, &cause);
		if (cause != NULL) {
			remote_log(r, "window @%u: bad layout: %s", id, layout);
		}

		free(cause);
		free(line);
		cause = NULL;
	}
}

static void
remote_bootstrap_next(struct remote *r, struct remote_query *q)
{
	struct options *oo;
	struct remote_bootstrap_ctx *ctx;

	ctx = (struct remote_bootstrap_ctx*) q;

	remote_log(r, "command response");

	switch (ctx->state++) {
	case 0:
		remote_show_environment(r, ctx->env, 0);
		return;
	case 1:
		remote_show_environment(r, ctx->env, ENVIRON_HIDDEN);
		oo = options_create(global_s_options);
		ctx->session = session_create(
		    NULL, ctx->session_name, "/tmp", ctx->env, oo, NULL);
		return;
	case 2:
		remote_add_panes(r, ctx);
		return;
	case 3:
		remote_fix_windows(r, ctx);
		break;
	}

	/* commit */
	r->session = ctx->session;
	r->windows = ctx->windows;
	r->panes = ctx->panes;
}

static void
remote_bootstrap_cancel(struct remote *r, struct remote_query *q)
{
	char *line = evbuffer_readln(r->reply_buffer, NULL, EVBUFFER_EOL_ANY);
	remote_log(r, "command returned error: %s", line);
	free(line);
}

/* The attached session was changed. */
static void
remote_session_changed(struct remote *r, u_int session_id, char *name)
{
	struct remote_bootstrap_ctx *ctx;

	if (r->session)
		session_destroy(r->session, 1, __func__);

	ctx = xcalloc(1, sizeof *ctx);
	ctx->q.command = "bootstrap";
	ctx->q.done = remote_bootstrap_next;
	ctx->q.error = remote_bootstrap_cancel;
	ctx->env = environ_create();
	ctx->session_name = xstrdup(name);
	RB_INIT(&ctx->windows);
	RB_INIT(&ctx->panes);

	/* XXX: reset inflight requests? */
	/* XXX: set timer? */
	remote_run(r, &ctx->q, "show-environment -t $%u;", session_id);
	remote_run(r, &ctx->q, "show-environment -ht $%u;", session_id);

	remote_run(r, &ctx->q, "list-panes -st $%u -F \"%s\";", session_id,
	    "#{window_id}\t"
	    "#{window_index}\t"
	    "#{window_width}\t"
	    "#{window_height}\t"
	    "#{pane_id}\t"
	    "#{pane_index}");

	remote_run(r, &ctx->q, "list-windows -t $%u -F \"%s\";", session_id,
	    "#{window_id}\t"
	    "#{window_name}\t"
	    "#{window_layout}\t"
	    "#{window_flags}\t"
	    "#{?window_active,1,0}");

	/* iterm: */
	/* kStateDictPaneId, kStateDictSavedGrid,
	   kStateDictAltSavedCX, kStateDictAltSavedCY, kStateDictCursorX,
	   kStateDictCursorY, kStateDictScrollRegionUpper,
	   kStateDictScrollRegionLower,
	   kStateDictTabstops, kStateDictCursorMode, kStateDictInsertMode,
           kStateDictKCursorMode,
	   kStateDictKKeypadMode, kStateDictWrapMode,
	   kStateDictMouseStandardMode,
	   kStateDictMouseButtonMode, kStateDictMouseAnyMode,
	   kStateDictMouseUTF8Mode, kStateDictMouseSGRMode ];
	*/

	/* capture-pane */
	/* show-options -g */
	bufferevent_write(r->event, "\n", 1);
	bufferevent_flush(r->event, EV_WRITE, BEV_FLUSH);
}

/* Replaces %output when flow control is enabled. */
static void
remote_extended_output(struct remote *r, u_int, uint64_t, char *)
{
}

/* A pane's mode was changed. */
static void
remote_pane_mode_changed(struct remote *r, u_int pane_id)
{
}

/* A window was renamed in the attached session. */
static void
remote_window_renamed(struct remote *r, u_int, char *)
{
}

/* A window was renamed in another session. */
static void
remote_unlinked_window_renamed(struct remote *r, u_int window_id, char *new_name)
{
}

/* A session was renamed. */
static void
remote_session_renamed(struct remote *r, u_int session_id, char *new_name)
{
}

/* Another client's attached session was changed. */
static void
remote_client_session_changed(struct remote *r, char *pty, u_int session_id, char *name)
{
}

/* A window's active pane changed. */
static void
remote_window_pane_changed(struct remote *r, u_int window_id, u_int pane_id)
{
}

/* A window was closed in the attached session. */
static void
remote_window_close(struct remote *r, u_int window_id)
{
}

/* A window was closed in another session. */
static void
remote_unlinked_window_close(struct remote *r, u_int window_id)
{
}

/* A window was added to the attached session. */
static void
remote_window_add(struct remote *r, u_int window_id)
{
}

/* A window was added to another session. */
static void
remote_unlinked_window_add(struct remote *r, u_int window_id)
{
}

/* A session's current window was changed. */
static void
remote_session_window_changed(struct remote *r, u_int session_id, u_int window_id)
{
}

/* A session was created or destroyed. */
static void
remote_sessions_changed(struct remote *r)
{
}

static void
remote_exit(struct remote *r)
{
	session_destroy(r->session, 1, __func__);
	r->session = NULL;
}

void
remote_destroy(struct remote *r)
{
	evbuffer_free(r->line_buffer);
	evbuffer_free(r->reply_buffer);
	bufferevent_free(r->event);
	session_destroy(r->session, 1, __func__);
}
