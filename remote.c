#include "tmux.h"

struct remote {
	struct session	   *s;
	struct bufferevent *event;
};

static void remote_init_dumb(struct remote *r);

struct remote *
remote_create(struct bufferevent *bev)
{
	struct remote  *r;
	struct environ *env;
	struct options *oo;

	r = xcalloc(1, sizeof *r);
	r->event = bev;

	/* XXX: request information about session */

	env = environ_create();
	oo = options_create(global_s_options);
	r->s = session_create("remote", "localhost", "/tmp", env, oo, NULL);
	remote_init_dumb(r);

	return r;
}

static void
remote_init_dumb(struct remote *r)
{
	struct spawn_context sc = {0};
	char		    *cause = NULL;
	u_int		     sx, sy, xpixel, ypixel;
	u_int 		     hlimit;
	struct window	    *w;
	struct window_pane  *new_wp;

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
	window_pane_set_event_nofd(new_wp, r->event);
	window_set_active_pane(w, new_wp, 0);
	layout_init(w, new_wp);
}

void
remote_destroy(struct remote *r)
{
	bufferevent_decref(r->event);
	session_destroy(r->s, 1, __func__);
}
