/*
 * nncmpp -- the MPD client you never knew you needed
 *
 * Copyright (c) 2016, Přemysl Janouch <p.janouch@gmail.com>
 * All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "config.h"

// We "need" to have an enum for attributes before including liberty.
// Avoiding colours in the defaults here in order to support dumb terminals.
#define ATTRIBUTE_TABLE(XX)                             \
	XX( HEADER,     "header",     -1, -1, 0           ) \
	XX( HIGHLIGHT,  "highlight",  -1, -1, A_BOLD      ) \
	/* Gauge                                         */ \
	XX( ELAPSED,    "elapsed",    -1, -1, A_REVERSE   ) \
	XX( REMAINS,    "remains",    -1, -1, A_UNDERLINE ) \
	/* Tab bar                                       */ \
	XX( TAB_BAR,    "tab_bar",    -1, -1, A_REVERSE   ) \
	XX( TAB_ACTIVE, "tab_active", -1, -1, A_UNDERLINE ) \
	/* Listview                                      */ \
	XX( EVEN,       "even",       -1, -1, 0           ) \
	XX( ODD,        "odd",        -1, -1, 0           ) \
	XX( SELECTION,  "selection",  -1, -1, A_REVERSE   ) \
	XX( SCROLLBAR,  "scrollbar",  -1, -1, 0           ) \
	/* These are for debugging only                  */ \
	XX( WARNING,    "warning",     3, -1, 0           ) \
	XX( ERROR,      "error",       1, -1, 0           ) \
	XX( INCOMING,   "incoming",    2, -1, 0           ) \
	XX( OUTGOING,   "outgoing",    4, -1, 0           )

enum
{
#define XX(name, config, fg_, bg_, attrs_) ATTRIBUTE_ ## name,
	ATTRIBUTE_TABLE (XX)
#undef XX
	ATTRIBUTE_COUNT
};

// My battle-tested C framework acting as a GLib replacement.  Its one big
// disadvantage is missing support for i18n but that can eventually be added
// as an optional feature.  Localised applications look super awkward, though.

// User data for logger functions to enable formatted logging
#define print_fatal_data    ((void *) ATTRIBUTE_ERROR)
#define print_error_data    ((void *) ATTRIBUTE_ERROR)
#define print_warning_data  ((void *) ATTRIBUTE_WARNING)

#define LIBERTY_WANT_POLLER
#define LIBERTY_WANT_ASYNC
#include "liberty/liberty.c"

#include <sys/un.h>
#include "mpd.c"

#include <locale.h>
#include <termios.h>
#ifndef TIOCGWINSZ
#include <sys/ioctl.h>
#endif  // ! TIOCGWINSZ
#include <ncurses.h>

// ncurses is notoriously retarded for input handling, we need something
// different if only to receive mouse events reliably.

#include "termo.h"

// It is surprisingly hard to find a good library to handle Unicode shenanigans,
// and there's enough of those for it to be impractical to reimplement them.
//
//                         GLib          ICU     libunistring    utf8proc
// Decently sized            .            .            x            x
// Grapheme breaks           .            x            .            x
// Character width           x            .            x            x
// Locale handling           .            .            x            .
// Liberal license           .            x            .            x
//
// Also note that the ICU API is icky and uses UTF-16 for its primary encoding.
//
// Currently we're chugging along with libunistring but utf8proc seems viable.
// Non-Unicode locales can mostly be handled with simple iconv like in sdtui.
// Similarly grapheme breaks can be guessed at using character width (a basic
// test here is Zalgo text).
//
// None of this is ever going to work too reliably anyway because terminals
// and Unicode don't go awfully well together.  In particular, character cell
// devices have some problems with double-wide characters.

#include <unistr.h>
#include <uniwidth.h>
#include <uniconv.h>

#define CTRL_KEY(x)  ((x) - 'A' + 1)

#define APP_TITLE  PROGRAM_NAME         ///< Left top corner

// --- Utilities ---------------------------------------------------------------

// The standard endwin/refresh sequence makes the terminal flicker
static void
update_curses_terminal_size (void)
{
#if defined (HAVE_RESIZETERM) && defined (TIOCGWINSZ)
	struct winsize size;
	if (!ioctl (STDOUT_FILENO, TIOCGWINSZ, (char *) &size))
	{
		char *row = getenv ("LINES");
		char *col = getenv ("COLUMNS");
		unsigned long tmp;
		resizeterm (
			(row && xstrtoul (&tmp, row, 10)) ? tmp : size.ws_row,
			(col && xstrtoul (&tmp, col, 10)) ? tmp : size.ws_col);
	}
#else  // HAVE_RESIZETERM && TIOCGWINSZ
	endwin ();
	refresh ();
#endif  // HAVE_RESIZETERM && TIOCGWINSZ
}

static int64_t
clock_msec (clockid_t clock)
{
	struct timespec tp;
	hard_assert (clock_gettime (clock, &tp) != -1);
	return (int64_t) tp.tv_sec * 1000 + (int64_t) tp.tv_nsec / 1000000;
}

// --- Application -------------------------------------------------------------

// Function names are prefixed mostly because of curses which clutters the
// global namespace and makes it harder to distinguish what functions relate to.

// The user interface is focused on conceptual simplicity.  That is important
// since we're not using any TUI framework (which are mostly a lost cause to me
// in the post-Unicode era and not worth pursuing), and the code would get
// bloated and incomprehensible fast.  We mostly rely on app_add_utf8_string()
// to write text from left to right row after row while keeping track of cells.
//
// There is an independent top pane displaying general status information,
// followed by a tab bar and a listview served by a per-tab event handler.
//
// For simplicity, the listview can only work with items that are one row high.

struct tab;
struct row_buffer;

/// Try to handle an event in the tab
typedef bool (*tab_event_fn) (struct tab *self, termo_key_t *event);

/// Draw an item to the screen using the row buffer API
typedef void (*tab_item_draw_fn) (struct tab *self,
	unsigned item_index, struct row_buffer *buffer, int width);

struct tab
{
	LIST_HEADER (struct tab)

	char *name;                         ///< Visible identifier
	size_t name_width;                  ///< Visible width of the name

	// Implementation:

	// TODO: free() callback?
	tab_event_fn on_event;              ///< Event handler callback
	tab_item_draw_fn on_item_draw;      ///< Item draw callback

	// Provided by tab owner:

	bool can_multiselect;               ///< Multiple items can be selected
	size_t item_count;                  ///< Total item count

	// Managed by the common handler:

	int item_top;                       ///< Index of the topmost item
	int item_selected;                  ///< Index of the selected item
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct attrs
{
	short fg;                           ///< Foreground colour index
	short bg;                           ///< Background colour index
	chtype attrs;                       ///< Other attributes
};

enum player_state { PLAYER_STOPPED, PLAYER_PLAYING, PLAYER_PAUSED };

// Basically a container for most of the globals; no big sense in handing
// around a pointer to this, hence it is a simple global variable as well.
// There is enough global state as it is.

static struct app_context
{
	// Event loop:

	struct poller poller;               ///< Poller
	bool quitting;                      ///< Quit signal for the event loop
	bool polling;                       ///< The event loop is running

	struct poller_fd tty_event;         ///< Terminal input event
	struct poller_fd signal_event;      ///< Signal FD event

	// Connection:

	struct mpd_client client;           ///< MPD client interface
	struct poller_timer connect_event;  ///< MPD reconnect timer

	enum player_state state;            ///< Player state
	struct str_map playback_info;       ///< Current song info

	struct poller_timer elapsed_event;  ///< Seconds elapsed event
	int64_t elapsed_since;              ///< Time of the next tick

	// TODO: initialize these to -1
	int song_elapsed;                   ///< Song elapsed in seconds
	int song_duration;                  ///< Song duration in seconds
	int volume;                         ///< Current volume

	// Data:

	struct config config;               ///< Program configuration

	struct tab *help_tab;               ///< Special help tab
	struct tab *tabs;                   ///< All other tabs
	struct tab *active_tab;             ///< Active tab

	// Emulated widgets:

	int header_height;                  ///< Height of the header

	int controls_offset;                ///< Offset to player controls or -1
	int gauge_offset;                   ///< Offset to the gauge or -1
	int gauge_width;                    ///< Width of the gauge, if present

	struct poller_idle refresh_event;   ///< Refresh the screen

	// Terminal:

	termo_t *tk;                        ///< termo handle
	struct poller_timer tk_timer;       ///< termo timeout timer
	bool locale_is_utf8;                ///< The locale is Unicode
	bool use_partial_boxes;             ///< Use Unicode box drawing chars

	struct attrs attrs[ATTRIBUTE_COUNT];
}
g_ctx;

/// Shortcut to retrieve named terminal attributes
#define APP_ATTR(name) g_ctx.attrs[ATTRIBUTE_ ## name].attrs

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
tab_init (struct tab *self, const char *name)
{
	memset (self, 0, sizeof *self);

	// Add some padding for decorative purposes
	self->name = xstrdup_printf (" %s ", name);
	// Assuming tab names are pure ASCII, otherwise this would be inaccurate
	// and we'd need to filter it first to replace invalid chars with '?'
	self->name_width = u8_strwidth ((uint8_t *) self->name, locale_charset ());
	self->item_selected = -1;
}

static void
tab_free (struct tab *self)
{
	free (self->name);
}

// --- Configuration -----------------------------------------------------------

static struct config_schema g_config_settings[] =
{
	{ .name      = "address",
	  .comment   = "Address to connect to the MPD server",
	  .type      = CONFIG_ITEM_STRING,
	  .default_  = "localhost" },
	{ .name      = "password",
	  .comment   = "Password to use for MPD authentication",
	  .type      = CONFIG_ITEM_STRING },
	{ .name      = "root",
	  .comment   = "Where all the files MPD is playing are located",
	  .type      = CONFIG_ITEM_STRING },
	{}
};

static struct config_schema g_config_colors[] =
{
#define XX(name_, config, fg_, bg_, attrs_) \
	{ .name = config, .type = CONFIG_ITEM_STRING },
	ATTRIBUTE_TABLE (XX)
#undef XX
	{}
};

static const char *
get_config_string (struct config_item *root, const char *key)
{
	struct config_item *item = config_item_get (root, key, NULL);
	hard_assert (item);
	if (item->type == CONFIG_ITEM_NULL)
		return NULL;
	hard_assert (config_item_type_is_string (item->type));
	return item->value.string.str;
}

/// Load configuration for a color using a subset of git config colors
static void
app_load_color (struct config_item *subtree, const char *name, int id)
{
	const char *value = get_config_string (subtree, name);
	if (!value)
		return;

	struct str_vector v;
	str_vector_init (&v);
	cstr_split_ignore_empty (value, ' ', &v);

	int colors = 0;
	struct attrs attrs = { -1, -1, 0 };
	for (char **it = v.vector; *it; it++)
	{
		char *end = NULL;
		long n = strtol (*it, &end, 10);
		if (*it != end && !*end && n >= SHRT_MIN && n <= SHRT_MAX)
		{
			if (colors == 0) attrs.fg = n;
			if (colors == 1) attrs.bg = n;
			colors++;
		}
		else if (!strcmp (*it, "bold"))    attrs.attrs |= A_BOLD;
		else if (!strcmp (*it, "dim"))     attrs.attrs |= A_DIM;
		else if (!strcmp (*it, "ul"))      attrs.attrs |= A_UNDERLINE;
		else if (!strcmp (*it, "blink"))   attrs.attrs |= A_BLINK;
		else if (!strcmp (*it, "reverse")) attrs.attrs |= A_REVERSE;
#ifdef A_ITALIC
		else if (!strcmp (*it, "italic"))  attrs.attrs |= A_ITALIC;
#endif  // A_ITALIC
	}
	str_vector_free (&v);
	g_ctx.attrs[id] = attrs;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
load_config_settings (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_settings, subtree, user_data);
}

static void
load_config_colors (struct config_item *subtree, void *user_data)
{
	config_schema_apply_to_object (g_config_colors,   subtree, user_data);

	// The attributes cannot be changed dynamically right now, so it doesn't
	// make much sense to make use of "on_change" callbacks either.
	// For simplicity, we should reload the entire table on each change anyway.
#define XX(name, config, fg_, bg_, attrs_) \
	app_load_color (subtree, config, ATTRIBUTE_ ## name);
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static void
app_load_configuration (void)
{
	struct config *config = &g_ctx.config;
	config_register_module (config, "settings", load_config_settings, NULL);
	config_register_module (config, "colors",   load_config_colors,   NULL);

	char *filename = resolve_filename
		(PROGRAM_NAME ".conf", resolve_relative_config_filename);
	if (!filename)
		return;

	struct error *e = NULL;
	struct config_item *root = config_read_from_file (filename, &e);
	free (filename);

	if (e)
	{
		print_error ("error loading configuration: %s", e->message);
		error_free (e);
		exit (EXIT_FAILURE);
	}
	if (root)
	{
		config_load (&g_ctx.config, root);
		config_schema_call_changed (g_ctx.config.root);
	}
}

// --- Application -------------------------------------------------------------

static void
app_init_attributes (void)
{
#define XX(name, config, fg_, bg_, attrs_)          \
	g_ctx.attrs[ATTRIBUTE_ ## name].fg    = fg_;    \
	g_ctx.attrs[ATTRIBUTE_ ## name].bg    = bg_;    \
	g_ctx.attrs[ATTRIBUTE_ ## name].attrs = attrs_;
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static void
app_init_context (void)
{
	memset (&g_ctx, 0, sizeof g_ctx);

	poller_init (&g_ctx.poller);
	mpd_client_init (&g_ctx.client, &g_ctx.poller);
	config_init (&g_ctx.config);

	// This is also approximately what libunistring does internally,
	// since the locale name is canonicalized by locale_charset().
	// Note that non-Unicode locales are handled pretty inefficiently.
	g_ctx.locale_is_utf8 = !strcasecmp_ascii (locale_charset (), "UTF-8");

	// It doesn't work 100% (e.g. incompatible with undelining in urxvt)
	// TODO: make this configurable
	g_ctx.use_partial_boxes = g_ctx.locale_is_utf8;

	app_init_attributes ();
}

static void
app_init_terminal (void)
{
	TERMO_CHECK_VERSION;
	if (!(g_ctx.tk = termo_new (STDIN_FILENO, NULL, 0)))
		abort ();
	if (!initscr () || nonl () == ERR)
		abort ();

	// Disable cursor, we're not going to use it most of the time
	curs_set (0);

	// By default we don't use any colors so they're not required...
	if (start_color () == ERR
	 || use_default_colors () == ERR
	 || COLOR_PAIRS <= ATTRIBUTE_COUNT)
		return;

	for (int a = 0; a < ATTRIBUTE_COUNT; a++)
	{
		// ...thus we can reset back to defaults even after initializing some
		if (g_ctx.attrs[a].fg >= COLORS || g_ctx.attrs[a].fg < -1
		 || g_ctx.attrs[a].bg >= COLORS || g_ctx.attrs[a].bg < -1)
		{
			app_init_attributes ();
			return;
		}

		init_pair (a + 1, g_ctx.attrs[a].fg, g_ctx.attrs[a].bg);
		g_ctx.attrs[a].attrs |= COLOR_PAIR (a + 1);
	}
}

static void
app_free_context (void)
{
	mpd_client_free (&g_ctx.client);
	str_map_free (&g_ctx.playback_info);

	config_free (&g_ctx.config);
	poller_free (&g_ctx.poller);

	if (g_ctx.tk)
		termo_destroy (g_ctx.tk);
}

static void
app_quit (void)
{
	g_ctx.quitting = true;

	// TODO: bring down the MPD interface (if that's needed at all);
	//   so far there's nothing for us to wait on, so let's just stop looping
	g_ctx.polling = false;
}

static bool
app_is_character_in_locale (ucs4_t ch)
{
	// Avoid the overhead joined with calling iconv() for all characters.
	if (g_ctx.locale_is_utf8)
		return true;

	// The library really creates a new conversion object every single time
	// and doesn't provide any smarter APIs.  Luckily, most users use UTF-8.
	size_t len;
	char *tmp = u32_conv_to_encoding (locale_charset (), iconveh_error,
		&ch, 1, NULL, NULL, &len);
	if (!tmp)
		return false;
	free (tmp);
	return true;
}

// --- Terminal output ---------------------------------------------------------

// Necessary abstraction to simplify aligned, formatted character output

struct row_char
{
	ucs4_t c;                           ///< Unicode codepoint
	chtype attrs;                       ///< Special attributes
	int width;                          ///< How many cells this takes
};

struct row_buffer
{
	struct row_char *chars;             ///< Characters
	size_t chars_len;                   ///< Character count
	size_t chars_alloc;                 ///< Characters allocated
	int total_width;                    ///< Total width of all characters
};

static void
row_buffer_init (struct row_buffer *self)
{
	memset (self, 0, sizeof *self);
	self->chars = xcalloc (sizeof *self->chars, (self->chars_alloc = 256));
}

static void
row_buffer_free (struct row_buffer *self)
{
	free (self->chars);
}

/// Replace invalid chars and push all codepoints to the array w/ attributes.
static void
row_buffer_append (struct row_buffer *self, const char *str, chtype attrs)
{
	// The encoding is only really used internally for some corner cases
	const char *encoding = locale_charset ();

	// Note that this function is a hotspot, try to keep it decently fast
	struct row_char current = { .attrs = attrs };
	struct row_char invalid = { .attrs = attrs, .c = '?', .width = 1 };
	const uint8_t *next = (const uint8_t *) str;
	while ((next = u8_next (&current.c, next)))
	{
		if (self->chars_len >= self->chars_alloc)
			self->chars = xreallocarray (self->chars,
				sizeof *self->chars, (self->chars_alloc <<= 1));

		current.width = uc_width (current.c, encoding);
		if (current.width < 0 || !app_is_character_in_locale (current.c))
			current = invalid;

		self->chars[self->chars_len++] = current;
		self->total_width += current.width;
	}
}

static void
row_buffer_addv (struct row_buffer *self, const char *s, ...)
	ATTRIBUTE_SENTINEL;

static void
row_buffer_addv (struct row_buffer *self, const char *s, ...)
{
	va_list ap;
	va_start (ap, s);

	while (s)
	{
		row_buffer_append (self, s, va_arg (ap, chtype));
		s = va_arg (ap, const char *);
	}
	va_end (ap);
}

/// Pop as many codepoints as needed to free up "space" character cells.
/// Given the suffix nature of combining marks, this should work pretty fine.
static int
row_buffer_pop_cells (struct row_buffer *self, int space)
{
	int made = 0;
	while (self->chars_len && made < space)
		made += self->chars[--self->chars_len].width;
	self->total_width -= made;
	return made;
}

static void
row_buffer_ellipsis (struct row_buffer *self, int target, chtype attrs)
{
	// TODO: get "attrs" from the last eaten item
	row_buffer_pop_cells (self, self->total_width - target);

	ucs4_t ellipsis = L'…';
	if (app_is_character_in_locale (ellipsis))
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 1);
		if (self->total_width + 1 <= target)
			row_buffer_append (self, "…", attrs);
	}
	else if (target >= 3)
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 3);
		if (self->total_width + 3 <= target)
			row_buffer_append (self, "...", attrs);
	}
}

static void
row_buffer_print (uint32_t *ucs4, chtype attrs)
{
	// Cannot afford to convert negative numbers to the unsigned chtype.
	uint8_t *str = (uint8_t *) u32_strconv_to_locale (ucs4);
	if (str)
	{
		for (uint8_t *p = str; *p; p++)
			addch (*p | attrs);
		free (str);
	}
}

static void
row_buffer_flush (struct row_buffer *self)
{
	if (!self->chars_len)
		return;

	// We only NUL-terminate the chunks because of the libunistring API
	uint32_t chunk[self->chars_len + 1], *insertion_point = chunk;
	for (size_t i = 0; i < self->chars_len; i++)
	{
		struct row_char *iter = self->chars + i;
		if (i && iter[0].attrs != iter[-1].attrs)
		{
			row_buffer_print (chunk, iter[-1].attrs);
			insertion_point = chunk;
		}
		*insertion_point++ = iter->c;
		*insertion_point = 0;
	}
	row_buffer_print (chunk, self->chars[self->chars_len - 1].attrs);
}

// --- Rendering ---------------------------------------------------------------

// TODO: rewrite this so that it's fine-grained but not complicated
static void
app_invalidate (void)
{
	poller_idle_set (&g_ctx.refresh_event);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Write the given UTF-8 string padded with spaces.
/// @param[in] attrs  Text attributes for the text, including padding.
static void
app_write_line (const char *str, chtype attrs)
{
	struct row_buffer buf;
	row_buffer_init (&buf);
	row_buffer_append (&buf, str, attrs);

	if (buf.total_width > COLS)
		row_buffer_ellipsis (&buf, COLS, attrs);

	row_buffer_flush (&buf);
	for (int i = buf.total_width; i < COLS; i++)
		addch (' ' | attrs);
	row_buffer_free (&buf);
}

/// Clear a row in the header to be used and increment the listview offset
static void
app_next_row (chtype attrs)
{
	mvwhline (stdscr, g_ctx.header_height++, 0, ' ' | attrs, COLS);
}

// We typically write here to a single buffer serving the entire line
static void
app_flush_buffer (struct row_buffer *buf, chtype attrs)
{
	if (buf->total_width > COLS)
		row_buffer_ellipsis (buf, COLS, attrs);

	app_next_row (attrs);
	row_buffer_flush (buf);
	row_buffer_free (buf);
}

static void
app_draw_song_info (void)
{
	// The map doesn't need to be initialized at all, so we need to check
	struct str_map *map = &g_ctx.playback_info;
	if (!soft_assert (map->len != 0))
		return;

	// XXX: can we get rid of this and still make it look acceptable?
	chtype a_normal    = APP_ATTR (HEADER);
	chtype a_highlight = APP_ATTR (HIGHLIGHT);

	char *title;
	if ((title = str_map_find (map, "title"))
	 || (title = str_map_find (map, "name"))
	 || (title = str_map_find (map, "file")))
	{
		struct row_buffer buf;
		row_buffer_init (&buf);
		row_buffer_append (&buf, title, a_highlight);
		app_flush_buffer (&buf, a_highlight);
	}

	char *artist = str_map_find (map, "artist");
	char *album  = str_map_find (map, "album");
	if (!artist && !album)
		return;

	struct row_buffer buf;
	row_buffer_init (&buf);

	if (artist)
	{
		if (buf.total_width)
			row_buffer_append (&buf, " ", a_normal);
		row_buffer_addv (&buf, "by ", a_normal, artist, a_highlight, NULL);
	}
	if (album)
	{
		if (buf.total_width)
			row_buffer_append (&buf, " ", a_normal);
		row_buffer_addv (&buf, "from ", a_normal, album, a_highlight, NULL);
	}
	app_flush_buffer (&buf, a_normal);
}

static void
app_write_time (struct row_buffer *buf, int seconds, chtype attrs)
{
	int minutes = seconds / 60; seconds %= 60;
	int hours   = minutes / 60; minutes %= 60;

	struct str s;
	str_init (&s);

	if (hours)
		str_append_printf (&s, "%d:%02d:", hours, minutes);
	else
		str_append_printf (&s, "%d:", minutes);

	str_append_printf (&s, "%02d", seconds);
	row_buffer_append (buf, s.str, attrs);
	str_free (&s);
}

static void
app_write_gauge (struct row_buffer *buf, float ratio, int width)
{
	if (ratio < 0) ratio = 0;
	if (ratio > 1) ratio = 1;

	// Always compute it in exactly eight times the resolution,
	// because sometimes Unicode is even useful
	int len_left = ratio * width * 8 + 0.5;

	static const char *partials[] = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };
	int remainder = len_left % 8;
	len_left /= 8;

	const char *partial = NULL;
	if (g_ctx.use_partial_boxes)
		partial = partials[remainder];
	else
		len_left += remainder >= (int) 4;

	int len_right = width - len_left;
	while (len_left-- > 0)
		row_buffer_append (buf, " ", APP_ATTR (ELAPSED));
	if (partial && len_right-- > 0)
		row_buffer_append (buf, partial, APP_ATTR (REMAINS));
	while (len_right-- > 0)
		row_buffer_append (buf, " ", APP_ATTR (REMAINS));
}

static void
app_draw_status (void)
{
	if (g_ctx.state != PLAYER_STOPPED)
		app_draw_song_info ();

	// XXX: can we get rid of this and still make it look acceptable?
	chtype a_normal    = APP_ATTR (HEADER);
	chtype a_highlight = APP_ATTR (HIGHLIGHT);

	struct row_buffer buf;
	row_buffer_init (&buf);

	bool stopped = g_ctx.state == PLAYER_STOPPED;

	chtype a_song_action = stopped ? a_normal : a_highlight;
	row_buffer_addv (&buf, "<<", a_song_action, " ", a_normal, NULL);
	if (g_ctx.state == PLAYER_PLAYING)
		row_buffer_addv (&buf, "||", a_highlight, " ", a_normal, NULL);
	else
		row_buffer_addv (&buf, "|>", a_highlight, " ", a_normal, NULL);
	row_buffer_addv (&buf, "[]", a_song_action, " ", a_normal, NULL);
	row_buffer_addv (&buf, ">>", a_song_action, "  ", a_normal, NULL);

	if (stopped)
		row_buffer_append (&buf, "Stopped", a_normal);
	else
	{
		if (g_ctx.song_elapsed >= 0)
		{
			app_write_time (&buf, g_ctx.song_elapsed, a_normal);
			row_buffer_append (&buf, " ", a_normal);
		}
		if (g_ctx.song_duration >= 1)
		{
			row_buffer_append (&buf, "/ ", a_normal);
			app_write_time (&buf, g_ctx.song_duration, a_normal);
			row_buffer_append (&buf, " ", a_normal);
		}
		row_buffer_append (&buf, " ", a_normal);
	}

	// It gets a bit complicated due to the only right-aligned item on the row
	char *volume = NULL;
	int remaining = COLS - buf.total_width;
	if (g_ctx.volume >= 0)
	{
		volume = xstrdup_printf ("  %3d%%", g_ctx.volume);
		remaining -= strlen (volume);
	}

	// TODO: store the coordinates of the progress bar
	if (!stopped && g_ctx.song_elapsed >= 0 && g_ctx.song_duration >= 1
	 && remaining > 0)
	{
		g_ctx.gauge_offset = buf.total_width;
		g_ctx.gauge_width = remaining;
		app_write_gauge (&buf,
			(float) g_ctx.song_elapsed / g_ctx.song_duration, remaining);
	}
	else while (remaining-- > 0)
		row_buffer_append (&buf, " ", a_normal);

	if (volume)
	{
		row_buffer_append (&buf, volume, a_normal);
		free (volume);
	}
	g_ctx.controls_offset = g_ctx.header_height;
	app_flush_buffer (&buf, a_normal);
}

static void
app_draw_header (void)
{
	// TODO: call app_fix_view_range() if it changes from the previous value
	g_ctx.header_height = 0;

	g_ctx.controls_offset = -1;
	g_ctx.gauge_offset = -1;
	g_ctx.gauge_width = 0;

	switch (g_ctx.client.state)
	{
	case MPD_CONNECTED:
		app_draw_status ();
		break;
	case MPD_CONNECTING:
		app_next_row (APP_ATTR (HEADER));
		app_write_line ("Connecting to MPD...", APP_ATTR (HEADER));
		break;
	case MPD_DISCONNECTED:
		app_next_row (APP_ATTR (HEADER));
		app_write_line ("Disconnected", APP_ATTR (HEADER));
	}

	// XXX: can we get rid of this and still make it look acceptable?
	chtype a_normal = APP_ATTR (TAB_BAR);
	chtype a_active = APP_ATTR (TAB_ACTIVE);

	struct row_buffer buf;
	row_buffer_init (&buf);

	// The help tab is disguised so that it's not too intruding
	row_buffer_append (&buf, APP_TITLE,
		g_ctx.active_tab == g_ctx.help_tab ? a_active : a_normal);
	row_buffer_append (&buf, " ", a_normal);

	LIST_FOR_EACH (struct tab, iter, g_ctx.tabs)
	{
		row_buffer_append (&buf, iter->name,
			iter == g_ctx.active_tab ? a_active : a_normal);
	}
	app_flush_buffer (&buf, a_normal);
}

static int
app_visible_items (void)
{
	// This may eventually include a header bar and/or a status bar
	return MAX (0, LINES - g_ctx.header_height);
}

static void
app_draw_scrollbar (void)
{
	// This assumes that we can write to the one-before-last column,
	// i.e. that it's not covered by any double-wide character (and that
	// ncurses comes to the right results when counting characters).
	//
	// We could also precompute the scrollbar and append it to each row
	// as we render them, plus all the unoccupied rows.
	struct tab *tab = g_ctx.active_tab;
	int visible_items = app_visible_items ();

	if (!g_ctx.use_partial_boxes)
	{
		// Apparently here we don't want the 0.5 rounding constant
		int length = (float) visible_items / (int) tab->item_count
			* (visible_items - 1);
		int start  = (float) tab->item_top / (int) tab->item_count
			* (visible_items - 1);

		for (int row = 0; row < visible_items; row++)
		{
			move (g_ctx.header_height + row, COLS - 1);
			if (row < start || row > start + length + 1)
				addch (' ' | APP_ATTR (SCROLLBAR));
			else
				addch (' ' | APP_ATTR (SCROLLBAR) | A_REVERSE);
		}
		return;
	}

	// TODO: clamp the values, make sure they follow the right order
	// We subtract half a character from both the top and the bottom, hence -1
	int length = (float) visible_items / (int) tab->item_count
		* (visible_items - 1) * 8 + 0.5;
	int start  = (float) tab->item_top / (int) tab->item_count
		* (visible_items - 1) * 8 + 0.5;

	// Then we make sure the bar is high at least one character, hence +8
	int end = start + length + 8;

	int start_part = start % 8; start /= 8;
	int end_part   = end   % 8; end   /= 8;

	// Even with this, the solid part must be at least one character high
	static const char *partials[] = { "█", "▇", "▆", "▅", "▄", "▃", "▂", "▁" };

	for (int row = 0; row < visible_items; row++)
	{
		chtype attrs = APP_ATTR (SCROLLBAR);
		if (row > start && row <= end)
			attrs ^= A_REVERSE;

		const char *c = " ";
		if (row == start) c = partials[start_part];
		if (row == end)   c = partials[end_part];

		move (g_ctx.header_height + row, COLS - 1);

		struct row_buffer buf;
		row_buffer_init (&buf);
		row_buffer_append (&buf, c, attrs);
		row_buffer_flush (&buf);
		row_buffer_free (&buf);
	}
}

static void
app_draw_view (void)
{
	move (g_ctx.header_height, 0);
	clrtobot ();

	struct tab *tab = g_ctx.active_tab;
	bool want_scrollbar = (int) tab->item_count > app_visible_items ();
	int view_width = COLS - want_scrollbar;

	int to_show = MIN (LINES - g_ctx.header_height,
		(int) tab->item_count - tab->item_top);
	for (int row = 0; row < to_show; row++)
	{
		int item_index = tab->item_top + row;
		int row_attrs = (item_index & 1) ? APP_ATTR (ODD) : APP_ATTR (EVEN);
		if (item_index == tab->item_selected)
			row_attrs = APP_ATTR (SELECTION);

		attrset (row_attrs);
		mvwhline (stdscr, g_ctx.header_height + row, 0, ' ', COLS);

		struct row_buffer buf;
		row_buffer_init (&buf);

		tab->on_item_draw (tab, item_index, &buf, view_width);
		if (item_index == tab->item_selected)
		{
			// Make it so that the selection color always wins
			for (size_t i = 0; i < buf.chars_len; i++)
				buf.chars[i].attrs &= ~(A_COLOR | A_REVERSE);
		}
		if (buf.total_width > view_width)
			row_buffer_ellipsis (&buf, view_width, row_attrs);

		row_buffer_flush (&buf);
		row_buffer_free (&buf);
	}
	attrset (0);

	if (want_scrollbar)
		app_draw_scrollbar ();
}

static void
app_on_refresh (void *user_data)
{
	(void) user_data;
	poller_idle_reset (&g_ctx.refresh_event);

	app_draw_header ();
	app_draw_view ();

	refresh ();
}

// --- Actions -----------------------------------------------------------------

/// Checks what items that are visible and returns if fixes were needed
static bool
app_fix_view_range (void)
{
	struct tab *tab = g_ctx.active_tab;
	if (tab->item_top < 0)
	{
		tab->item_top = 0;
		app_invalidate ();
		return false;
	}

	// If the contents are at least as long as the screen, always fill it
	int max_item_top = (int) tab->item_count - app_visible_items ();
	// But don't let that suggest a negative offset
	max_item_top = MAX (max_item_top, 0);

	if (tab->item_top > max_item_top)
	{
		tab->item_top = max_item_top;
		app_invalidate ();
		return false;
	}
	return true;
}

/// Scroll down (positive) or up (negative) @a n items
static bool
app_scroll (int n)
{
	g_ctx.active_tab->item_top += n;
	app_invalidate ();
	return app_fix_view_range ();
}

static void
app_ensure_selection_visible (void)
{
	struct tab *tab = g_ctx.active_tab;
	if (tab->item_selected < 0)
		return;

	int too_high = tab->item_top - tab->item_selected;
	if (too_high > 0)
		app_scroll (-too_high);

	int too_low = tab->item_selected
		- (tab->item_top + app_visible_items () - 1);
	if (too_low > 0)
		app_scroll (too_low);
}

static bool
app_move_selection (int diff)
{
	struct tab *tab = g_ctx.active_tab;
	int fixed = tab->item_selected += diff;
	fixed = MAX (fixed, 0);
	fixed = MIN (fixed, (int) tab->item_count - 1);

	bool result = tab->item_selected != fixed;
	tab->item_selected = fixed;
	app_invalidate ();

	app_ensure_selection_visible ();
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
app_switch_tab (struct tab *tab)
{
	g_ctx.active_tab = tab;
	app_invalidate ();
}

static bool
app_goto_tab (int tab_index)
{
	int i = 0;
	LIST_FOR_EACH (struct tab, iter, g_ctx.tabs)
		if (i++ == tab_index)
		{
			app_switch_tab (iter);
			return true;
		}
	return false;
}

// --- User input handling -----------------------------------------------------

#define USER_ACTIONS(XX) \
	XX( NONE,               "Do nothing"              ) \
	\
	XX( QUIT,               "Quit application"        ) \
	XX( REDRAW,             "Redraw screen"           ) \
	\
	XX( MPD_PREVIOUS,       "Previous song"           ) \
	XX( MPD_TOGGLE,         "Toggle play/pause"       ) \
	XX( MPD_STOP,           "Stop playback"           ) \
	XX( MPD_NEXT,           "Next song"               ) \
	XX( MPD_VOLUME_UP,      "Increase volume"         ) \
	XX( MPD_VOLUME_DOWN,    "Decrease volume"         ) \
	\
	XX( SCROLL_UP,          "Scroll up"               ) \
	XX( SCROLL_DOWN,        "Scroll down"             ) \
	\
	XX( GOTO_TOP,           "Go to the top"           ) \
	XX( GOTO_BOTTOM,        "Go to the bottom"        ) \
	XX( GOTO_ITEM_PREVIOUS, "Go to the previous item" ) \
	XX( GOTO_ITEM_NEXT,     "Go to the next item"     ) \
	XX( GOTO_PAGE_PREVIOUS, "Go to the previous page" ) \
	XX( GOTO_PAGE_NEXT,     "Go to the next page"     )

enum user_action
{
#define XX(name, description) USER_ACTION_ ## name,
	USER_ACTIONS (XX)
#undef XX
	USER_ACTION_COUNT
};

static struct user_action_info
{
	const char *name;                   ///< Name for user bindings
	const char *description;            ///< Human-readable description
}
g_user_actions[] =
{
#define XX(name, description) { #name, description },
	USER_ACTIONS (XX)
#undef XX
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#define MPD_SIMPLE(...)                             \
{                                                   \
	if (c->state != MPD_CONNECTED)                  \
		break;                                      \
	mpd_client_send_command (c, __VA_ARGS__, NULL); \
	mpd_client_add_task (c, NULL, NULL);            \
	mpd_client_idle (c, 0);                         \
}

static bool
app_process_user_action (enum user_action action)
{
	struct mpd_client *c = &g_ctx.client;
	struct tab *tab = g_ctx.active_tab;
	switch (action)
	{
	case USER_ACTION_QUIT:
		return false;
	case USER_ACTION_REDRAW:
		clear ();
		app_invalidate ();
		return true;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_MPD_PREVIOUS:
		MPD_SIMPLE ("previous")
		return true;
	case USER_ACTION_MPD_TOGGLE:
		if      (g_ctx.state == PLAYER_PLAYING) MPD_SIMPLE ("pause", "1")
		else if (g_ctx.state == PLAYER_PAUSED)  MPD_SIMPLE ("pause", "0")
		else                                    MPD_SIMPLE ("play")
		return true;
	case USER_ACTION_MPD_STOP:
		MPD_SIMPLE ("stop")
		return true;
	case USER_ACTION_MPD_NEXT:
		MPD_SIMPLE ("next")
		return true;
	case USER_ACTION_MPD_VOLUME_UP:
		if (g_ctx.volume >= 0)
		{
			char *volume = xstrdup_printf ("%d", MIN (100, g_ctx.volume + 10));
			MPD_SIMPLE ("setvol", volume)
			free (volume);
		}
		return true;
	case USER_ACTION_MPD_VOLUME_DOWN:
		if (g_ctx.volume >= 0)
		{
			char *volume = xstrdup_printf ("%d", MAX (0, g_ctx.volume - 10));
			MPD_SIMPLE ("setvol", volume)
			free (volume);
		}
		return true;

	// TODO: relative seeks
#if 0
		MPD_SIMPLE (forward,  "seekcur", "+10", NULL)
		MPD_SIMPLE (backward, "seekcur", "-10", NULL)
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		// XXX: these should rather be parametrized
	case USER_ACTION_SCROLL_UP:
		app_scroll (-3);
		return true;
	case USER_ACTION_SCROLL_DOWN:
		app_scroll (3);
		return true;

	case USER_ACTION_GOTO_TOP:
		if (tab->item_count)
		{
			g_ctx.active_tab->item_selected = 0;
			app_ensure_selection_visible ();
			app_invalidate ();
		}
		return true;
	case USER_ACTION_GOTO_BOTTOM:
		if (tab->item_count)
		{
			g_ctx.active_tab->item_selected =
				(int) g_ctx.active_tab->item_count - 1;
			app_ensure_selection_visible ();
			app_invalidate ();
		}
		return true;

	case USER_ACTION_GOTO_ITEM_PREVIOUS:
		app_move_selection (-1);
		return true;
	case USER_ACTION_GOTO_ITEM_NEXT:
		app_move_selection (1);
		return true;

	case USER_ACTION_GOTO_PAGE_PREVIOUS:
		app_scroll ((int) g_ctx.header_height - LINES);
		app_move_selection ((int) g_ctx.header_height - LINES);
		return true;
	case USER_ACTION_GOTO_PAGE_NEXT:
		app_scroll (LINES - (int) g_ctx.header_height);
		app_move_selection (LINES - (int) g_ctx.header_height);
		return true;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_NONE:
		return true;
	default:
		hard_assert (!"unhandled user action");
	}
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
app_process_left_mouse_click (int line, int column)
{
	if (line == g_ctx.controls_offset)
	{
		// XXX: there could be a push_widget(buf, text, attrs, handler)
		//   function to help with this but it might not be worth it
		enum user_action action = USER_ACTION_NONE;
		if (column >= 0 && column <=  1) action = USER_ACTION_MPD_PREVIOUS;
		if (column >= 3 && column <=  4) action = USER_ACTION_MPD_TOGGLE;
		if (column >= 6 && column <=  7) action = USER_ACTION_MPD_STOP;
		if (column >= 9 && column <= 10) action = USER_ACTION_MPD_NEXT;
		if (action)
		{
			app_process_user_action (action);
			return;
		}

		int gauge_offset = column - g_ctx.gauge_offset;
		if (g_ctx.gauge_offset >= 0
		 && gauge_offset >= 0 && gauge_offset < g_ctx.gauge_width)
		{
			float position = (float) gauge_offset / g_ctx.gauge_width;
			struct mpd_client *c = &g_ctx.client;
			if (c->state == MPD_CONNECTED && g_ctx.song_duration >= 1)
			{
				char *where = xstrdup_printf
					("%f", position * g_ctx.song_duration);
				mpd_client_send_command (c, "seekcur", where, NULL);
				free (where);

				mpd_client_add_task (c, NULL, NULL);
				mpd_client_idle (c, 0);
			}
			return;
		}
	}
	else if (line == g_ctx.header_height - 1)
	{
		struct tab *winner = NULL;
		int indent = strlen (APP_TITLE);
		if (column < indent)
		{
			app_switch_tab (g_ctx.help_tab);
			return;
		}
		for (struct tab *iter = g_ctx.tabs; !winner && iter; iter = iter->next)
		{
			if (column < (indent += iter->name_width))
				winner = iter;
		}
		if (winner)
			app_switch_tab (winner);
	}
	else
	{
		struct tab *tab = g_ctx.active_tab;
		int row_index = line - g_ctx.header_height;
		if (row_index < 0
		 || row_index >= (int) tab->item_count - tab->item_top)
			return;

		// TODO: handle the scrollbar a bit better than this
		int visible_items = app_visible_items ();
		if ((int) tab->item_count > visible_items && column == COLS - 1)
		{
			tab->item_top = (float) row_index / visible_items
				* (int) tab->item_count - visible_items / 2;
			app_fix_view_range ();
		}
		else
			tab->item_selected = row_index + tab->item_top;
		app_invalidate ();
	}
}

static bool
app_process_mouse (termo_key_t *event)
{
	int line, column, button;
	termo_mouse_event_t type;
	termo_interpret_mouse (g_ctx.tk, event, &type, &button, &line, &column);

	if (type != TERMO_MOUSE_PRESS)
		return true;

	if (button == 1)
		app_process_left_mouse_click (line, column);
	else if (button == 4)
		app_process_user_action (USER_ACTION_SCROLL_UP);
	else if (button == 5)
		app_process_user_action (USER_ACTION_SCROLL_DOWN);

	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct binding
{
	const char *key;                    ///< Key definition
	enum user_action action;            ///< Action to take
}
g_default_bindings[] =
{
	{ "Escape",     USER_ACTION_QUIT               },
	{ "C-l",        USER_ACTION_REDRAW             },

	{ "Home",       USER_ACTION_GOTO_TOP           },
	{ "End",        USER_ACTION_GOTO_BOTTOM        },
	{ "M-<",        USER_ACTION_GOTO_TOP           },
	{ "M->",        USER_ACTION_GOTO_BOTTOM        },
	{ "Up",         USER_ACTION_GOTO_ITEM_PREVIOUS },
	{ "Down",       USER_ACTION_GOTO_ITEM_NEXT     },
	{ "k",          USER_ACTION_GOTO_ITEM_PREVIOUS },
	{ "j",          USER_ACTION_GOTO_ITEM_NEXT     },
	{ "PageUp",     USER_ACTION_GOTO_PAGE_PREVIOUS },
	{ "PageDown",   USER_ACTION_GOTO_PAGE_NEXT     },
	{ "C-p",        USER_ACTION_GOTO_ITEM_PREVIOUS },
	{ "C-n",        USER_ACTION_GOTO_ITEM_NEXT     },
	{ "C-b",        USER_ACTION_GOTO_PAGE_PREVIOUS },
	{ "C-f",        USER_ACTION_GOTO_PAGE_NEXT     },

	// Not sure how to set these up, they're pretty arbitrary so far
	{ "Left",       USER_ACTION_MPD_PREVIOUS       },
	{ "Right",      USER_ACTION_MPD_NEXT           },
	{ "h",          USER_ACTION_MPD_PREVIOUS       },
	{ "l",          USER_ACTION_MPD_NEXT           },
	{ "Space",      USER_ACTION_MPD_TOGGLE         },
	{ "C-Space",    USER_ACTION_MPD_STOP           },
	{ "M-PageUp",   USER_ACTION_MPD_VOLUME_UP      },
	{ "M-PageDown", USER_ACTION_MPD_VOLUME_DOWN    },
};

static bool
app_process_termo_event (termo_key_t *event)
{
	if (event->type == TERMO_TYPE_MOUSE)
		return app_process_mouse (event);

	// TODO: pre-parse the keys, order them by termo_keycmp() and binary search
	for (size_t i = 0; i < N_ELEMENTS (g_default_bindings); i++)
	{
		struct binding *binding = &g_default_bindings[i];
		termo_key_t key;
		hard_assert (!*termo_strpkey_utf8 (g_ctx.tk, binding->key, &key,
			TERMO_FORMAT_ALTISMETA));
		if (!termo_keycmp (g_ctx.tk, event, &key))
			return app_process_user_action (binding->action);
	}

	// TODO: parametrize actions, put this among other bindings
	if (event->modifiers == TERMO_KEYMOD_ALT
	 && event->code.codepoint >= '0'
	 && event->code.codepoint <= '9')
	{
		int n = event->code.codepoint - '0';
		if (!app_goto_tab ((n == 0 ? 10 : n) - 1))
			beep ();
		return true;
	}
	return true;
}

// --- Info tab ----------------------------------------------------------------

// TODO: either find something else to put in here or remove the wrapper struct
static struct
{
	struct tab super;                   ///< Parent class
}
g_info_tab;

static void
info_tab_on_item_draw (struct tab *self, unsigned item_index,
	struct row_buffer *buffer, int width)
{
	(void) self;
	(void) width;

	// TODO
}

static void
info_tab_update (void)
{
	// TODO: add some entries from "playback_info" to the listview
}

static struct tab *
info_tab_init (void)
{
	struct tab *super = &g_info_tab.super;
	tab_init (super, "Info");
	super->on_item_draw = info_tab_on_item_draw;
	super->item_count = 0;
	super->item_selected = 0;
	return super;
}

// --- Help tab ----------------------------------------------------------------

// TODO: either find something else to put in here or remove the wrapper struct
static struct
{
	struct tab super;                   ///< Parent class
}
g_help_tab;

static void
help_tab_on_item_draw (struct tab *self, unsigned item_index,
	struct row_buffer *buffer, int width)
{
	(void) self;
	(void) width;

	// TODO: group them the other way around for clarity
	hard_assert (item_index < N_ELEMENTS (g_default_bindings));
	struct binding *binding = &g_default_bindings[item_index];
	char *text = xstrdup_printf ("%-12s %s",
		binding->key, g_user_actions[binding->action].description);
	row_buffer_append (buffer, text, 0);
	free (text);
}

static struct tab *
help_tab_init (void)
{
	struct tab *super = &g_help_tab.super;
	tab_init (super, "Help");
	super->on_item_draw = help_tab_on_item_draw;
	super->item_count = N_ELEMENTS (g_default_bindings);
	super->item_selected = 0;
	return super;
}

// --- Debug tab ---------------------------------------------------------------

struct debug_item
{
	char *text;                         ///< Logged line
	int64_t timestamp;                  ///< Timestamp
	chtype attrs;                       ///< Line attributes
};

static struct
{
	struct tab super;                   ///< Parent class
	struct debug_item *items;           ///< Items
	size_t items_alloc;                 ///< How many items are allocated
	bool active;                        ///< The tab is present
}
g_debug_tab;

static void
debug_tab_on_item_draw (struct tab *self, unsigned item_index,
	struct row_buffer *buffer, int width)
{
	(void) self;

	hard_assert (item_index <= g_debug_tab.super.item_count);
	struct debug_item *item = &g_debug_tab.items[item_index];

	char buf[16];
	struct tm tm;
	time_t when = item->timestamp / 1000;
	strftime (buf, sizeof buf, "%T", localtime_r (&when, &tm));

	char *prefix = xstrdup_printf
		("%s.%03d", buf, (int) (item->timestamp % 1000));
	row_buffer_append (buffer, prefix, 0);
	free (prefix);

	row_buffer_append (buffer, " ", item->attrs);
	row_buffer_append (buffer, item->text, item->attrs);

	// We override the formatting including colors -- do it for the whole line
	if (buffer->total_width > width)
		row_buffer_ellipsis (buffer, width, item->attrs);
	while (buffer->total_width < width)
		row_buffer_append (buffer, " ", item->attrs);
}

static void
debug_tab_push (const char *message, chtype attrs)
{
	// TODO: uh... aren't we rather going to write our own abstraction?
	if (g_debug_tab.items_alloc <= g_debug_tab.super.item_count)
	{
		g_debug_tab.items = xreallocarray (g_debug_tab.items,
			sizeof *g_debug_tab.items, (g_debug_tab.items_alloc <<= 1));
	}

	// TODO: there should be a better, more efficient mechanism for this
	struct debug_item *item =
		&g_debug_tab.items[g_debug_tab.super.item_count++];
	item->text = xstrdup (message);
	item->attrs = attrs;
	item->timestamp = clock_msec (CLOCK_REALTIME);

	app_invalidate ();
}

static struct tab *
debug_tab_init (void)
{
	g_debug_tab.items = xcalloc
		((g_debug_tab.items_alloc = 16), sizeof *g_debug_tab.items);
	g_debug_tab.active = true;

	struct tab *super = &g_debug_tab.super;
	tab_init (super, "Debug");
	super->on_item_draw = debug_tab_on_item_draw;
	super->item_count = 0;
	super->item_selected = 0;
	return super;
}

// --- MPD interface -----------------------------------------------------------

// TODO: this entire thing has been slavishly copy-pasted from dwmstatus
// TODO: try to move some of this code to mpd.c

static void
mpd_update_playback_state (void)
{
	struct str_map *map = &g_ctx.playback_info;

	const char *state;
	g_ctx.state = PLAYER_PLAYING;
	if ((state = str_map_find (map, "state")))
	{
		if (!strcmp (state, "stop"))
			g_ctx.state = PLAYER_STOPPED;
		if (!strcmp (state, "pause"))
			g_ctx.state = PLAYER_PAUSED;
	}

	// The contents of these values overlap and we try to get what we can
	// FIXME: don't change the values, for fuck's sake
	char *time     = str_map_find (map, "time");
	char *duration = str_map_find (map, "duration");
	char *elapsed  = str_map_find (map, "elapsed");
	if (time)
	{
		char *colon = strchr (time, ':');
		if (colon)
		{
			*colon = '\0';
			duration = colon + 1;
		}
	}

	unsigned long tmp;
	if (time     && xstrtoul (&tmp, time,     10))
		g_ctx.song_elapsed  = tmp;
	if (duration && xstrtoul (&tmp, duration, 10))
		g_ctx.song_duration = tmp;

	// We could also just poll the server each half a second but let's not
	int msec_past_second = 0;

	char *period;
	if (elapsed && (period = strchr (elapsed, '.')))
	{
		// For some reason this is much more precise
		*period++ = '\0';
		if (xstrtoul (&tmp, elapsed, 10))
			g_ctx.song_elapsed = tmp;

		if (xstrtoul (&tmp, period, 10))
			msec_past_second = tmp;
	}
	if (g_ctx.state == PLAYER_PLAYING)
	{
		poller_timer_set (&g_ctx.elapsed_event, 1000 - msec_past_second);
		g_ctx.elapsed_since = clock_msec (CLOCK_BEST) - msec_past_second;
	}

	// The server sends -1 when nothing is being played right now
	char *volume = str_map_find (map, "volume");
	if (volume && xstrtoul (&tmp, volume, 10))
		g_ctx.volume = tmp;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Sometimes it's not that easy and there can be repeating entries
static void
mpd_vector_to_map (const struct str_vector *data, struct str_map *map)
{
	str_map_init (map);
	map->key_xfrm = tolower_ascii_strxfrm;
	map->free = free;

	char *key, *value;
	for (size_t i = 0; i < data->len; i++)
	{
		if ((key = mpd_client_parse_kv (data->vector[i], &value)))
			str_map_set (map, key, xstrdup (value));
		else
			print_debug ("%s: %s", "erroneous MPD output", data->vector[i]);
	}
}

static void
mpd_on_info_response (const struct mpd_response *response,
	const struct str_vector *data, void *user_data)
{
	(void) user_data;

	// TODO: do this also on disconnect
	g_ctx.song_elapsed = -1;
	g_ctx.song_duration = -1;
	g_ctx.volume = -1;
	str_map_free (&g_ctx.playback_info);
	poller_timer_reset (&g_ctx.elapsed_event);
	// TODO: preset an error player state?

	if (response->success)
	{
		// Note that we may receive a "time" field twice, however the right
		// one wins here due to the order we send the commands in
		struct str_map map;
		mpd_vector_to_map (data, &map);
		g_ctx.playback_info = map;
	}
	else
	{
		print_debug ("%s: %s",
			"retrieving MPD info failed", response->message_text);
	}

	mpd_update_playback_state ();
	info_tab_update ();
	app_invalidate ();
}

static void
mpd_on_tick (void *user_data)
{
	(void) user_data;
	int64_t diff_msec = clock_msec (CLOCK_BEST) - g_ctx.elapsed_since;
	int elapsed_sec = diff_msec / 1000;
	int elapsed_msec = diff_msec % 1000;

	g_ctx.song_elapsed += elapsed_sec;
	g_ctx.elapsed_since += elapsed_sec * 1000;
	poller_timer_set (&g_ctx.elapsed_event, 1000 - elapsed_msec);

	app_invalidate ();
}

static void
mpd_request_info (void)
{
	struct mpd_client *c = &g_ctx.client;

	mpd_client_list_begin (c);
	mpd_client_send_command (c, "currentsong", NULL);
	mpd_client_send_command (c, "status", NULL);
	mpd_client_list_end (c);
	mpd_client_add_task (c, mpd_on_info_response, NULL);

	mpd_client_idle (c, 0);
}

static void
mpd_on_events (unsigned subsystems, void *user_data)
{
	(void) user_data;
	struct mpd_client *c = &g_ctx.client;

	if (subsystems & (MPD_SUBSYSTEM_PLAYER
		| MPD_SUBSYSTEM_PLAYLIST | MPD_SUBSYSTEM_MIXER))
		mpd_request_info ();
	else
		mpd_client_idle (c, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_queue_reconnect (void)
{
	poller_timer_set (&g_ctx.connect_event, 5 * 1000);
}

static void
mpd_on_password_response (const struct mpd_response *response,
	const struct str_vector *data, void *user_data)
{
	(void) data;
	(void) user_data;
	struct mpd_client *c = &g_ctx.client;

	if (response->success)
		mpd_request_info ();
	else
	{
		print_error ("%s: %s",
			"couldn't authenticate to MPD", response->message_text);
		mpd_client_send_command (c, "close", NULL);
	}
}

static void
mpd_on_connected (void *user_data)
{
	(void) user_data;
	struct mpd_client *c = &g_ctx.client;

	const char *password =
		get_config_string (g_ctx.config.root, "settings.password");
	if (password)
	{
		mpd_client_send_command (c, "password", password, NULL);
		mpd_client_add_task (c, mpd_on_password_response, NULL);
	}
	else
		mpd_request_info ();
}

static void
mpd_on_failure (void *user_data)
{
	(void) user_data;
	// This is also triggered both by a failed connect and a clean disconnect
	print_error ("connection to MPD failed");
	mpd_queue_reconnect ();
}

static void
mpd_on_io_hook (void *user_data, bool outgoing, const char *line)
{
	(void) user_data;

	struct str s;
	str_init (&s);
	if (outgoing)
	{
		str_append_printf (&s, "<< %s", line);
		debug_tab_push (s.str, APP_ATTR (OUTGOING));
	}
	else
	{
		str_append_printf (&s, ">> %s", line);
		debug_tab_push (s.str, APP_ATTR (INCOMING));
	}
	str_free (&s);
}

static void
app_on_reconnect (void *user_data)
{
	(void) user_data;

	struct mpd_client *c = &g_ctx.client;
	c->on_failure   = mpd_on_failure;
	c->on_connected = mpd_on_connected;
	c->on_event     = mpd_on_events;

	if (g_debug_mode)
		c->on_io_hook = mpd_on_io_hook;

	// We accept hostname/IPv4/IPv6 in pseudo-URL format, as well as sockets
	char *address = xstrdup (get_config_string (g_ctx.config.root,
		"settings.address")), *p = address, *host = address, *port = "6600";

	// Unwrap IPv6 addresses in format_host_port_pair() format
	char *right_bracket = strchr (p, ']');
	if (p[0] == '[' && right_bracket)
	{
		*right_bracket = '\0';
		host = p + 1;
		p = right_bracket + 1;
	}

	char *colon = strchr (p, ':');
	if (colon)
	{
		*colon = '\0';
		port = colon + 1;
	}

	struct error *e = NULL;
	if (!mpd_client_connect (c, host, port, &e))
	{
		print_error ("%s: %s", "cannot connect to MPD", e->message);
		error_free (e);
		mpd_queue_reconnect ();
	}
	free (address);
}

// --- Signals -----------------------------------------------------------------

static int g_signal_pipe[2];            ///< A pipe used to signal... signals

/// Program termination has been requested by a signal
static volatile sig_atomic_t g_termination_requested;
/// The window has changed in size
static volatile sig_atomic_t g_winch_received;

static void
signals_postpone_handling (char id)
{
	int original_errno = errno;
	if (write (g_signal_pipe[1], &id, 1) == -1)
		soft_assert (errno == EAGAIN);
	errno = original_errno;
}

static void
signals_superhandler (int signum)
{
	switch (signum)
	{
	case SIGWINCH:
		g_winch_received = true;
		signals_postpone_handling ('w');
		break;
	case SIGINT:
	case SIGTERM:
		g_termination_requested = true;
		signals_postpone_handling ('t');
		break;
	default:
		hard_assert (!"unhandled signal");
	}
}

static void
signals_setup_handlers (void)
{
	if (pipe (g_signal_pipe) == -1)
		exit_fatal ("%s: %s", "pipe", strerror (errno));

	set_cloexec (g_signal_pipe[0]);
	set_cloexec (g_signal_pipe[1]);

	// So that the pipe cannot overflow; it would make write() block within
	// the signal handler, which is something we really don't want to happen.
	// The same holds true for read().
	set_blocking (g_signal_pipe[0], false);
	set_blocking (g_signal_pipe[1], false);

	signal (SIGPIPE, SIG_IGN);

	struct sigaction sa;
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = signals_superhandler;
	sigemptyset (&sa.sa_mask);

	if (sigaction (SIGWINCH, &sa, NULL) == -1
	 || sigaction (SIGINT,   &sa, NULL) == -1
	 || sigaction (SIGTERM,  &sa, NULL) == -1)
		exit_fatal ("sigaction: %s", strerror (errno));
}

// --- Initialisation, event handling ------------------------------------------

static void
app_on_tty_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;
	if (fd->revents & ~(POLLIN | POLLHUP | POLLERR))
		print_debug ("fd %d: unexpected revents: %d", fd->fd, fd->revents);

	poller_timer_reset (&g_ctx.tk_timer);
	termo_advisereadable (g_ctx.tk);

	termo_key_t event;
	termo_result_t res;
	while ((res = termo_getkey (g_ctx.tk, &event)) == TERMO_RES_KEY)
		if (!app_process_termo_event (&event))
		{
			app_quit ();
			return;
		}

	if (res == TERMO_RES_AGAIN)
		poller_timer_set (&g_ctx.tk_timer, termo_get_waittime (g_ctx.tk));
	else if (res == TERMO_RES_ERROR || res == TERMO_RES_EOF)
	{
		app_quit ();
		return;
	}
}

static void
app_on_key_timer (void *user_data)
{
	(void) user_data;

	termo_key_t event;
	if (termo_getkey_force (g_ctx.tk, &event) == TERMO_RES_KEY)
		if (!app_process_termo_event (&event))
			app_quit ();
}

static void
app_on_signal_pipe_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;

	char id = 0;
	(void) read (fd->fd, &id, 1);

	if (g_termination_requested && !g_ctx.quitting)
		app_quit ();

	if (g_winch_received)
	{
		update_curses_terminal_size ();
		app_fix_view_range ();
		app_invalidate ();

		g_winch_received = false;
	}
}

static void
app_log_handler (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	// We certainly don't want to end up in a possibly infinite recursion
	static bool in_processing;
	if (in_processing)
		return;

	in_processing = true;

	struct str message;
	str_init (&message);
	str_append (&message, quote);
	str_append_vprintf (&message, fmt, ap);

	// If the standard error output isn't redirected, try our best at showing
	// the message to the user; it will probably get overdrawn soon
	// TODO: remember it somewhere so that it stays shown for a while
	if (!isatty (STDERR_FILENO))
		fprintf (stderr, "%s\n", message.str);
	else if (g_debug_tab.active)
	{
		debug_tab_push (message.str,
			user_data == NULL ? 0 : g_ctx.attrs[(intptr_t) user_data].attrs);
	}
	else
	{
		// TODO: remember the position and attributes and restore them
		attrset (A_REVERSE);
		mvwhline (stdscr, LINES - 1, 0, A_REVERSE, COLS);
		app_write_line (message.str, 0);
		attrset (0);
	}
	str_free (&message);

	in_processing = false;
}

static void
app_init_poller_events (void)
{
	poller_fd_init (&g_ctx.signal_event, &g_ctx.poller, g_signal_pipe[0]);
	g_ctx.signal_event.dispatcher = app_on_signal_pipe_readable;
	poller_fd_set (&g_ctx.signal_event, POLLIN);

	poller_fd_init (&g_ctx.tty_event, &g_ctx.poller, STDIN_FILENO);
	g_ctx.tty_event.dispatcher = app_on_tty_readable;
	poller_fd_set (&g_ctx.tty_event, POLLIN);

	poller_timer_init (&g_ctx.tk_timer, &g_ctx.poller);
	g_ctx.tk_timer.dispatcher = app_on_key_timer;

	poller_timer_init (&g_ctx.connect_event, &g_ctx.poller);
	g_ctx.connect_event.dispatcher = app_on_reconnect;
	poller_timer_set (&g_ctx.connect_event, 0);

	poller_timer_init (&g_ctx.elapsed_event, &g_ctx.poller);
	g_ctx.elapsed_event.dispatcher = mpd_on_tick;

	poller_idle_init (&g_ctx.refresh_event, &g_ctx.poller);
	g_ctx.refresh_event.dispatcher = app_on_refresh;
}

int
main (int argc, char *argv[])
{
	static const struct opt opts[] =
	{
		{ 'd', "debug", NULL, 0, "run in debug mode" },
		{ 'h', "help", NULL, 0, "display this help and exit" },
		{ 'V', "version", NULL, 0, "output version information and exit" },
		{ 0, NULL, NULL, 0, NULL }
	};

	struct opt_handler oh;
	opt_handler_init (&oh, argc, argv, opts, NULL, "MPD client.");

	int c;
	while ((c = opt_handler_get (&oh)) != -1)
	switch (c)
	{
	case 'd':
		g_debug_mode = true;
		break;
	case 'h':
		opt_handler_usage (&oh, stdout);
		exit (EXIT_SUCCESS);
	case 'V':
		printf (PROGRAM_NAME " " PROGRAM_VERSION "\n");
		exit (EXIT_SUCCESS);
	default:
		print_error ("wrong options");
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}

	argc -= optind;
	argv += optind;

	if (argc)
	{
		opt_handler_usage (&oh, stderr);
		exit (EXIT_FAILURE);
	}
	opt_handler_free (&oh);

	// We only need to convert to and from the terminal encoding
	if (!setlocale (LC_CTYPE, ""))
		print_warning ("failed to set the locale");

	app_init_context ();
	app_load_configuration ();
	app_init_terminal ();

	// Redirect all messages from liberty to a special tab so they're not lost
	struct tab *new_tab;
	if (g_debug_mode)
	{
		new_tab = debug_tab_init ();
		LIST_PREPEND (g_ctx.tabs, new_tab);
	}

	g_log_message_real = app_log_handler;

	new_tab = info_tab_init ();
	LIST_PREPEND (g_ctx.tabs, new_tab);

	g_ctx.help_tab = help_tab_init ();
	g_ctx.active_tab = g_ctx.help_tab;

	signals_setup_handlers ();
	app_init_poller_events ();
	app_invalidate ();

	g_ctx.polling = true;
	while (g_ctx.polling)
		poller_run (&g_ctx.poller);

	endwin ();
	g_log_message_real = log_message_stdio;
	app_free_context ();
	return 0;
}
