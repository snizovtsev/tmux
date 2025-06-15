/* $OpenBSD$ */

/*
 * Copyright (c) 2025 Sergei Nizovtsev <snizovtsev@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <string.h>

#include "tmux.h"

static struct screen *window_remote_init(struct window_mode_entry *,
		    struct cmd_find_state *, struct args *);

static void	window_remote_free(struct window_mode_entry *);
static void	window_remote_resize(struct window_mode_entry *, u_int, u_int);
static void	window_remote_update(struct window_mode_entry *);
static void	window_remote_formats(struct window_mode_entry *,
		    struct format_tree *);
static void	window_remote_key(struct window_mode_entry *, struct client *,
		     struct session *, struct winlink *, key_code,
		     struct mouse_event *);
static void	window_remote_draw(struct window_mode_entry *);

const struct window_mode window_remote_mode = {
	.name = "remote-mode",

	.init = window_remote_init,
	.free = window_remote_free,
	.resize = window_remote_resize,
	.update = window_remote_update,
	.formats = window_remote_formats,
	.key = window_remote_key,
};

struct window_remote_mode_data {
	/* XXX: 2 modes - locked to pane (exit - terminate), floating (socket) */
	struct remote *r;
	struct screen screen;
	struct screen log;

	u_int		 oy;		/* number of lines scrolled up */
};

static struct screen *
window_remote_init(struct window_mode_entry *wme,
    struct cmd_find_state *fs, struct args *args)
{
	struct window_pane		*wp = wme->wp;
	struct window_remote_mode_data	*data = xcalloc(1, sizeof *data);
	u_int				 sx = screen_size_x(&wp->base);
	u_int				 sy = screen_size_y(&wp->base);

	wme->data = data;
	data->r = fs->r;

	screen_init(&data->log, sx - 2 , sy - 4, 10000);
	screen_init(&data->screen, sx, sy, 0);

	window_remote_draw(wme);

	return (&data->screen);
}

static void
window_remote_free(struct window_mode_entry *wme)
{
	struct window_remote_mode_data	*data = wme->data;

	remote_set_pane(data->r, NULL, 0);
	screen_free(&data->screen);
	free(data);
}

static void
window_remote_resize(struct window_mode_entry *wme, u_int sx, u_int sy)
{
	struct window_remote_mode_data	*data = wme->data;
	struct screen			*s = &data->screen;

	screen_resize(s, sx, sy, 0);
	screen_resize(&data->log, sx - 2, sy - 4, 0);
	window_remote_draw(wme);
}

static void
window_remote_update(struct window_mode_entry *wme)
{
	struct window_remote_mode_data *data = wme->data;

	window_remote_draw(wme);
}

static void
window_remote_formats(struct window_mode_entry *wme, struct format_tree *ft)
{
	format_add(ft, "remote_format_test", "%s", "format_test_123");
	// #events
	// #inflight_requests[N]
	// #last_error
	// #sync_state
}

static void
window_remote_key(struct window_mode_entry *wme, struct client *c,
    __unused struct session *s, __unused struct winlink *wl, key_code key,
    __unused struct mouse_event *m)
{
	struct window_pane		*wp = wme->wp;
	struct window_remote_mode_data	*data = wme->data;

	switch (key) {
	case 'd':
		if (data->oy < data->log.grid->hsize) {
			data->oy += 1;
			window_remote_draw(wme);
		}
		break;
	case 'u':
		if (data->oy > 0) {
			data->oy -= 1;
			window_remote_draw(wme);
		}
		break;
	case 'q':
	case '\033': /* Escape */
	case 'g'|KEYC_CTRL:
		break;
	}
}

void window_remote_vadd(struct window_pane *wp, int, const char *fmt, va_list ap)
{
	struct window_mode_entry	*wme = TAILQ_FIRST(&wp->modes);
	struct window_remote_mode_data	*data = wme ? wme->data : NULL;
	struct screen			*s = &data->log;
	struct grid_cell		 gc;
	struct screen_write_ctx		 ctx;

	if (wme == NULL || wme->mode != &window_remote_mode)
		return;

	memcpy(&gc, &grid_default_cell, sizeof gc);

	//screen_write_start_pane(&ctx, wp, s);
	screen_write_start(&ctx, s);
	screen_write_vnputs(&ctx, -1, &gc, fmt, ap);
	screen_write_linefeed(&ctx, 0, gc.bg);
	screen_write_carriagereturn(&ctx);
	screen_write_stop(&ctx);

	data->oy = s->grid->hsize;
	window_remote_draw(wme);
}

static void
window_remote_draw(struct window_mode_entry *wme)
{
	struct window_pane	       *wp = wme->wp;
	struct window_remote_mode_data *data = wme->data;
	struct screen		       *s = &data->screen;
	struct options		       *oo = wp->options;
	struct options_array_item      *a = NULL;
	struct options_entry	       *oe;
	struct grid_cell		gc;
	struct screen_write_ctx		ctx;
	struct format_tree	       *ft;
	const char		       *fmt;
	char			       *expanded;

	memcpy(&gc, &grid_default_cell, sizeof gc);

	ft = format_create_defaults(NULL, NULL, NULL, NULL, wp);

	screen_write_start_pane(&ctx, wp, s);
	screen_write_clearscreen(&ctx, 8);
	screen_write_cursormove(&ctx, 0, 0, 0);

	oe = options_get(oo, "remote-dashboard-format");
	if (oe)
		a = options_array_first(oe);

	for (; a != NULL; a = options_array_next(a)) {
		fmt = options_array_item_value(a)->string;
		expanded = format_expand(ft, fmt);
		//format_free(ft);
		format_draw(&ctx, &gc, screen_size_x(s), expanded, NULL, 0);
		screen_write_carriagereturn(&ctx);
		screen_write_linefeed(&ctx, 0, 8);
		free(expanded);
	}

	screen_write_box(&ctx, screen_size_x(s), screen_size_y(s) - s->cy,
			 BOX_LINES_DEFAULT, &gc, "#[align=centre] #[underscore]l#[default]og "); /* s->title */

	screen_write_cursordown(&ctx, 1);
	screen_write_cursorright(&ctx, 1);

	screen_write_fast_copy(&ctx, &data->log, 0, data->oy,
	    screen_size_x(&data->log) - 2, screen_size_y(&data->log));

	screen_write_stop(&ctx);
}
