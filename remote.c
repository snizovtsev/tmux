#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "compat/container_of.h"

// temporary zig testing hack
#define static

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

	/* XXX: keep track of next_window_id */
	u_int		      session_id;
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

struct client_pane {
	struct client_window cw;
	struct bufferevent  *event;
	u_int init_cx, init_cy;
	u_int alt;
};

struct try_scanf_ctx {
	const char *line;
	size_t	    len;
	int	    mark;
	char	   *rest;
};

struct remote_bootstrap_ctx {
	struct remote_query    q;
	struct environ	      *env;
	int		       state;
	u_int		       session_id;
	const char	      *session_name;
	struct session	      *session;
	struct client_windows  windows;
	struct client_windows  panes;
	struct client_window  *cw;
};

struct remote_input_ctx {
	struct remote	   *r;
	struct bufferevent *event;
	uint32_t	    pane_id;
};

/* Helper functions. */
static int scanflike(3, 0) try_sscanf(struct try_scanf_ctx *, int, const char *, ...);
static void printflike(2, 0) remote_log(struct remote *, const char *, ...);
static size_t	 output_unescape(const char *, char *);
static int	 evbuffer_remove_line(struct evbuffer *, struct evbuffer *);
static char	*evbuffer_peek_string(struct evbuffer *, size_t *);

static void	 remote_input(struct bufferevent *, void *);
static void	 remote_read_callback(struct bufferevent *, void *);
static void	 remote_read_line(struct remote *, struct evbuffer *);
static struct remote_query *printflike(3, 0)
    remote_run(struct remote *, struct remote_query *, const char *, ...);
static void	remote_bootstrap_next(struct remote *, struct remote_query *);

/* Protocol handlers. */
static void	remote_begin_reply(struct remote *, u_char *);
static void	remote_dispatch_reply(struct remote *, u_char *, int);
static void	remote_dispatch_event(struct remote *, u_char *, size_t len);
static void	remote_output(struct remote *, u_int, char *);
static void	remote_extended_output(struct remote *, u_int, uint64_t, char *);
static void	remote_pane_mode_changed(struct remote *, u_int);
static void	remote_session_changed(struct remote *, u_int, char *);
static void	remote_window_renamed(struct remote *, u_int, char *);
static void	remote_unlinked_window_renamed(struct remote *, u_int, char *);
static void	remote_session_renamed(struct remote *, u_int, char *);
static void	remote_client_session_changed(struct remote *, char *, u_int, char *);
static void	remote_window_pane_changed(struct remote *, u_int, u_int);
static void	remote_window_close(struct remote *, u_int);
static void	remote_unlinked_window_close(struct remote *, u_int);
static void	remote_window_add(struct remote *, u_int);
static void	remote_unlinked_window_add(struct remote *, u_int);
static void	remote_session_window_changed(struct remote *, u_int, u_int);
static void	remote_sessions_changed(struct remote *);
static void	remote_exit(struct remote *);

static void
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

static struct remote_query*
remote_run(struct remote *r, struct remote_query* q, const char *fmt, ...)
{
	va_list ap;

	if (TAILQ_LAST(&r->queries, remote_quries) != q)
		TAILQ_INSERT_TAIL(&r->queries, q, entry);
	q->arity++;

	log_debug("%s: %s: %s", __func__, q->command, fmt);

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
	size_t		    eol_len;
	struct evbuffer_ptr eol;

	eol = evbuffer_search_eol(src, NULL, &eol_len, EVBUFFER_EOL_CRLF);
	if (eol.pos < 0)
		return (0);

	evbuffer_remove_buffer(src, dst, eol.pos);
	evbuffer_drain(src, eol_len);
	evbuffer_add(dst, "", 1);

	return (1);
}

static char *
evbuffer_peek_string(struct evbuffer *buffer, size_t *n_read_out)
{
	size_t total_len = evbuffer_get_length(buffer);
	ssize_t eol;

	if (total_len == 0)
		return NULL;

	eol = evbuffer_search_eol(buffer, NULL, NULL, EVBUFFER_EOL_NUL).pos;
	if (eol < 0) {
		eol = total_len;
		evbuffer_add(buffer, "", 1);
	}

	if (n_read_out)
		*n_read_out = eol + 1;
	return evbuffer_pullup(buffer, eol + 1);
}

static void
remote_read_callback(struct bufferevent *bev, void *data)
{
	struct remote	*r = data;
	struct evbuffer *input = bufferevent_get_input(bev);
	struct evbuffer *line = r->line_buffer;

	/* read stream line-by-line */
	while (evbuffer_remove_line(input, line)) {
		remote_read_line(r, line);
		evbuffer_drain(line, EV_SIZE_MAX);
	}

	/* collect incomplete line, resetting bev high watermark */
	bufferevent_read_buffer(bev, line);
}

static void
remote_read_line(struct remote *r, struct evbuffer *buffer)
{
	u_char *line = EVBUFFER_DATA(buffer);
	size_t	len = EVBUFFER_LENGTH(buffer);

	log_debug("%s: reply=%u %.*s", __func__, r->reply_number, (u_int) len - 1, line);

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

	log_debug("%s: %u %lu", __func__, number, time);
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

	log_debug("%s: %u %lu %u", __func__, number, time, flags);

	if (flags & 1) /* client-originated command */ {
		q = TAILQ_FIRST(&r->queries);
		if (q == NULL) {
			remote_log(r, "error: no requests pending");
			return;
		}

		complete = error ? q->error : q->done;
		if (complete)
			complete(r, q);

		if (--q->arity == 0) {
			TAILQ_REMOVE(&r->queries, q, entry);
			free(q);
		}
	}

	evbuffer_drain(r->reply_buffer, EV_SIZE_MAX);
}

static int
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

	/* TODO: %subscription-changed */
	/* TODO: %pause */

	free(ss.rest);
	free(client);
}

static size_t
output_unescape(const char *in, char *out)
{
	size_t len = 0;
	int octal;

	for (; *in; ++in, ++len)
		switch (*in) {
		case '\\':
			/* Read exactly three bytes of octal values. */
			if ((in[1] & ~7) == '0' && (in[2] & ~7) == '0' &&
			    (in[3] & ~7) == '0') {
				octal = (in[1] & 7) << 6 | (in[2] & 7) << 3 |
				    (in[3] & 7);
				if (octal < 256) {
					out[len] = octal;
					in += 3;
					break;
				}
			} else if (in[1] == '\\') {
				out[len] = in[1];
				in += 1;
				break;
			}
			/* FALLTHROUGH */
		case 0 ... 31:
			log_debug("%s: malformed input: %.10s", __func__, in);
			/* FALLTHROUGH */
		default:
			out[len] = *in;
		}

	out[len] = '\0';
	return len;
}

static void
remote_output(struct remote *r, u_int pane_id, char *data)
{
	struct client_window *cw = RB_FIND(client_windows, &r->panes,
	    &(struct client_window){ .window = pane_id });
	struct client_pane   *cp =
	      cw ? container_of(cw, struct client_pane, cw) : NULL;
	size_t		      len;

	if (cp == NULL) {
		remote_log(r, "%s: no such pane: %u", __func__, pane_id);
		return;
	}

	len = output_unescape(data, data);
	bufferevent_write(cp->event, data, len);
	bufferevent_flush(cp->event, EV_WRITE, BEV_FLUSH);
}

static void
remote_input(struct bufferevent *kev, void *ctx)
{
	struct remote_input_ctx *ictx = ctx;
	struct remote		*r = ictx->r;
	struct remote_query	*q = xcalloc(1, sizeof *q);
	const char		*keys = EVBUFFER_DATA(kev->input);
	size_t			 i, n = EVBUFFER_LENGTH(kev->input);
	char			*hex = xmalloc(3 * n);

	for (i = 0; i < n; ++i)
		sprintf(hex + (i * 3), "%02X ", keys[i]);
	hex[3 * n] = '\0';
	evbuffer_drain(kev->input, n);

	/* q->error = remote_shutdown_because_of_error; */
	remote_run(r, q, "send-keys -t %%%u -lH %s\n", ictx->pane_id, hex);
	free(hex);
	bufferevent_flush(r->event, EV_WRITE, BEV_FLUSH);
}

static void
remote_show_environment(struct remote *r, struct environ *env, int flags)
{
	struct evbuffer *reply = r->reply_buffer;
	size_t		 n_read_out;
	char		*line;

	while ((line = evbuffer_peek_string(reply, &n_read_out))) {
		if (line[0] != '-') {
			environ_put(env, line, flags);
		} else {
			if (flags)
				environ_set(env, line + 1, flags, "");
			environ_clear(env, line + 1);
		}
		evbuffer_drain(reply, n_read_out);
	}
}

static void
remote_add_panes(struct remote *r, struct remote_bootstrap_ctx *ctx)
{
	struct session	     *s = ctx->session;
	struct evbuffer	     *reply = r->reply_buffer;
	struct window	     *w;
	struct winlink	     *wl;
	struct client_window *cw;
	struct client_pane   *cp;
	struct window_pane   *wp = NULL;
	struct remote_input_ctx *rictx;
	struct bufferevent   *pipe[2];
	size_t			 n_read_out;
	char			*line, *iter;
	u_int		      window_id, pane_id;
	u_int		      window_index, pane_index;
	u_int		      cx, cy, sx, sy, active, hsize, hlimit;

	while ((line = evbuffer_peek_string(reply, &n_read_out))) {
		remote_log(r, "pane: %s", line);

		iter = line;
		window_id = atol(strsep(&iter, "\t") + /*@*/1);
		window_index = atol(strsep(&iter, "\t"));
		sx = atol(strsep(&iter, "\t"));
		sy = atol(strsep(&iter, "\t"));
		pane_id = atol(strsep(&iter, "\t") + /*%*/1);
		pane_index = atol(strsep(&iter, "\t"));
		active = atol(strsep(&iter, "\t"));
		cx = atol(strsep(&iter, "\t"));
		cy = atol(strsep(&iter, "\t"));
		hlimit = atol(strsep(&iter, "\t"));

		cw = RB_FIND(client_windows, &ctx->windows,
		    &(struct client_window){.window = window_id});

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

		wp = window_add_pane(w, wp, hlimit, 0);
		if (cw == NULL)
			layout_init(w, wp);

		if (active || cw == NULL)
			w->active = wp;

		if (cw == NULL) {
			cw = xcalloc(1, sizeof *cw);
			cw->window = window_id;
			cw->pane = wp;
			RB_INSERT(client_windows, &ctx->windows, cw);
		}

		bufferevent_pair_new(NULL, 0, pipe);
		window_pane_set_event_nofd(wp, pipe[1]);

		rictx = xcalloc(1, sizeof *rictx);
		rictx->r = r;
		rictx->pane_id = pane_id;
		bufferevent_setcb(pipe[0], remote_input, NULL, NULL, rictx);
		bufferevent_enable(pipe[0], EV_READ);

		cp = xcalloc(1, sizeof *cp);
		cp->cw.window = pane_id;
		cp->cw.pane = wp;
		cp->init_cx = cx;
		cp->init_cy = cy;
		cp->event = pipe[0];

		RB_INSERT(client_windows, &ctx->panes, &cp->cw);

		evbuffer_drain(reply, n_read_out);
	}
}

static void
remote_fix_windows(struct remote *r, struct remote_bootstrap_ctx *ctx)
{
	struct evbuffer	     *reply = r->reply_buffer;
	struct window	     *w;
	struct client_window *cw;
	size_t		      n_read_out;
	u_int		      id, active;
	char		     *line, *iter, *name, *layout, *flags;
	char		     *cause = NULL;

	while ((line = evbuffer_peek_string(reply, &n_read_out))) {
		remote_log(r, "window: %s", line);
		iter = line;

		id = atol(strsep(&iter, "\t") + /*@*/1);
		name = strsep(&iter, "\t");
		layout = strsep(&iter, "\t");
		flags = strsep(&iter, "\t");
		active = atoi(strsep(&iter, "\t"));

		cw = RB_FIND(client_windows, &ctx->windows,
		    &(struct client_window){.window = id});
		w = cw->pane->window;

		if (active)
			ctx->session->curw = TAILQ_FIRST(&w->winlinks);

		cause = NULL;
		layout_parse(w, layout, &cause);
		if (cause != NULL) {
			remote_log(r, "window @%u: bad layout: %s", id, layout);
			free(cause);
		}

		// XXX: populate_history broke naming
		free(w->name);
		utf8_stravis(&w->name, name, VIS_OCTAL|VIS_CSTYLE|VIS_TAB|VIS_NL);

		evbuffer_drain(reply, n_read_out);
	}
}

static void
remote_populate_history(struct remote *r, struct remote_bootstrap_ctx *ctx)
{
	struct evbuffer	   *reply = r->reply_buffer;
	struct evbuffer	   *history = evbuffer_new();
	struct client_pane *cp = container_of(ctx->cw, struct client_pane, cw);
	struct window_pane *wp = ctx->cw->pane;
	struct grid *tmp;
	size_t		    len, n_read_out;
	char		   *text;

	remote_log(r, "populate_history %%%u", ctx->cw->window);

	while ((text = evbuffer_peek_string(reply, &n_read_out))) {
		len = output_unescape(text, text);
		evbuffer_remove_buffer(reply, history, len);
		evbuffer_drain(reply, n_read_out - len);
		if (evbuffer_get_length(reply))
			evbuffer_add(history, "\r\n", 2);
	}

	text = EVBUFFER_DATA(history);
	len = EVBUFFER_LENGTH(history);

	if (cp->alt) {
		wp->screen->saved_grid = wp->screen->grid;
		wp->screen->grid = grid_create(
		    screen_size_x(wp->screen), screen_size_y(wp->screen), 0);

		input_parse_buffer(wp, text, len);

		tmp = wp->screen->saved_grid;
		wp->screen->saved_grid = wp->screen->grid;
		wp->screen->grid = tmp;
	} else {
		input_parse_buffer(wp, text, len);
	}

	evbuffer_free(history);


	if (cp->alt) {
		ctx->cw = RB_NEXT(client_windows, &r->panes, ctx->cw);
		wp->base.cx = cp->init_cx;
		wp->base.cy = cp->init_cy;
	} else {
		cp->alt = 1;
	}
}

static void
remote_request_history(struct remote *r, struct remote_query *q, struct remote_bootstrap_ctx *ctx)
{
	struct client_window	    *cw = NULL;

	ctx->cw = RB_MIN(client_windows, &ctx->panes);
	RB_FOREACH(cw, client_windows, &ctx->panes) {
		remote_run(r, q,
		    "capture-pane -peqCJN -S -%u -t %%%u ; ",
		    screen_hlimit(&cw->pane->base), cw->window);
		remote_run(r, q,
		    "capture-pane -apeqCJN -S -%u -t %%%u\n",
		    screen_hlimit(&cw->pane->base), cw->window);
	}
	bufferevent_flush(r->event, EV_WRITE, BEV_FLUSH);
}

static void
remote_bootstrap_next(struct remote *r, struct remote_query *q)
{
	struct options		    *oo;
	struct remote_bootstrap_ctx *ctx;

	ctx = (struct remote_bootstrap_ctx*) q;

	remote_log(r, "bootstrap_next from state = %u", ctx->state);

	switch (ctx->state++) {
	case 0:
		remote_show_environment(r, ctx->env, 0);
		break;
	case 1:
		remote_show_environment(r, ctx->env, ENVIRON_HIDDEN);
		oo = options_create(global_s_options);
		ctx->session = session_create(
		    NULL, ctx->session_name, "/tmp", ctx->env, oo, NULL);
		ctx->session->remote = r;
		break;
	case 2:
		remote_add_panes(r, ctx);
		remote_request_history(r, q, ctx);
		break;
	case 3:
		remote_fix_windows(r, ctx);
		break;
	default:
		remote_populate_history(r, ctx);
		break;
	}

	if (q->arity == 1) {
		remote_log(r, "bootstrap finished");

		/* commit */
		r->session_id = ctx->session_id;
		r->session = ctx->session;
		r->windows = ctx->windows;
		r->panes = ctx->panes;

		server_redraw_session(r->session);
	}
}

static void
remote_bootstrap_error(struct remote *r, struct remote_query *q)
{
	struct evbuffer *reply = r->reply_buffer;
	const char *msg = evbuffer_peek_string(reply, NULL);
	remote_log(r, "bootstrap failed: %s: %s", q->command, msg);
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
	ctx->q.error = remote_bootstrap_error;
	ctx->env = environ_create();
	ctx->session_id = session_id;
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
	    "#{pane_index}\t"
	    "#{pane_active}\t"
	    "#{cursor_x}\t"
	    "#{cursor_y}\t"
	    "#{history_limit}");
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
remote_window_renamed(struct remote *r, u_int window_id, char *new_name)
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
remote_client_session_changed(__unused struct remote *r, __unused char *pty,
    __unused u_int session_id, __unused char *name)
{}

/* A window's active pane changed. */
static void
remote_window_pane_changed(struct remote *r, u_int window_id, u_int pane_id)
{
	struct client_window *cp, *cw;
	struct window_pane   *wp = NULL;
	struct window	     *w = NULL;

	cp = RB_FIND(client_windows, &r->panes,
	    &(struct client_window){ .window = pane_id });
	cw = RB_FIND(client_windows, &r->windows,
	    &(struct client_window){ .window = window_id });

	if (cp == NULL || cw == NULL) {
		remote_log(
		    r, "%s %u %u: no such pane", __func__, window_id, pane_id);
		return;
	}

	wp = cp->pane;
	w = cw->pane->window;

	if (wp->window == w)
		window_set_active_pane(wp->window, wp, 0);
	else
		remote_log(r, "%s %u %u: pane and window is out of sync",
		    __func__, window_id, pane_id);
}

/* A window was closed in the attached session. */
static void
remote_window_close(struct remote *r, u_int window_id)
{
	struct client_window *cw;
	struct window	     *w;

	cw = RB_FIND(client_windows, &r->windows,
	    &(struct client_window){.window = window_id});

	if (cw == NULL) {
		remote_log(r, "%s: window @%u not found", __func__, window_id);
		return;
	}

	w = cw->pane->window;
	cw->pane = NULL; /* tombstone */

	RB_FOREACH(cw, client_windows, &r->panes) {
		if (cw->pane->window == w) {
			server_kill_pane(cw->pane);
			cw->pane = NULL; /* tombstone */
		}
	}
}

/* A window was closed in another session. */
static void
remote_unlinked_window_close(__unused struct remote *r, __unused u_int window_id)
{
}

/* A window was added to the attached session. */
static void
remote_window_add(struct remote *r, u_int window_id)
{
}

/* A window was added to another session. */
static void
remote_unlinked_window_add(__unused struct remote *r, __unused u_int window_id)
{
}

/* A session's current window was changed. */
static void
remote_session_window_changed(struct remote *r, u_int session_id, u_int window_id)
{
	struct client_window *cw;
	struct winlink	     *wl;

	if (session_id != r->session_id)
		return;

	cw = RB_FIND(client_windows, &r->windows,
	    &(struct client_window){.window = window_id});

	/* maybe newly-created window, silently ignore */
	if (cw == NULL)
		return;

	wl = TAILQ_FIRST(&cw->pane->window->winlinks);
	session_sync_current(r->session, wl);
	server_redraw_session(r->session);
}

/* A session was created or destroyed. */
static void
remote_sessions_changed(__unused struct remote *r)
{
}

void
remote_notify_window_pane_changed(struct remote *r, struct window *w)
{
	struct remote_query  *q;
	struct client_window *cw = NULL;

	RB_FOREACH(cw, client_windows, &r->panes) {
		if (cw->pane == w->active)
			break;
	}

	remote_log(r, "select-pane -t %%%u", cw->window);

	q = xcalloc(1, sizeof *q);
	remote_run(r, q, "select-pane -t %%%u\n", cw->window);
	bufferevent_flush(r->event, EV_WRITE, BEV_FLUSH);
}

void
remote_notify_session_window_changed(struct remote *r)
{
	struct remote_query  *q;
	struct client_window *cw = NULL;

	RB_FOREACH(cw, client_windows, &r->windows) {
		if (cw->pane->window == r->session->curw->window)
			break;
	}

	q = xcalloc(1, sizeof *q);
	remote_run(r, q, "select-window -t @%u\n", cw->window);
	remote_log(r, "select-window -t @%u", cw->window);
	bufferevent_flush(r->event, EV_WRITE, BEV_FLUSH);
}

void
remote_notify_window_layout_changed(struct remote *r, struct window *w)
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
