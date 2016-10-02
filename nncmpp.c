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

// My battle-tested C framework acting as a GLib replacement.  Its one big
// disadvantage is missing support for i18n but that can eventually be added
// as an optional feature.  Localised applications look super awkward, though.

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

// --- Application -------------------------------------------------------------

// Function names are prefixed mostly because of curses which clutters the
// global namespace and makes it harder to distinguish what functions relate to.

// Avoiding colours in the defaults here in order to support dumb terminals
#define ATTRIBUTE_TABLE(XX)                             \
	XX( HEADER,     "header",     -1, -1, 0           ) \
	XX( HIGHLIGHT,  "highlight",  -1, -1, A_BOLD      ) \
	\
	XX( ELAPSED,    "elapsed",    -1, -1, A_REVERSE   ) \
	XX( REMAINS,    "remains",    -1, -1, A_UNDERLINE ) \
	\
	XX( TAB_BAR,    "tab_bar",    -1, -1, A_REVERSE   ) \
	XX( TAB_ACTIVE, "tab_active", -1, -1, A_UNDERLINE ) \
	\
	XX( EVEN,       "even",       -1, -1, 0           ) \
	XX( ODD,        "odd",        -1, -1, 0           ) \
	XX( SELECTION,  "selection",  -1, -1, A_REVERSE   )

enum
{
#define XX(name, config, fg_, bg_, attrs_) ATTRIBUTE_ ## name,
	ATTRIBUTE_TABLE (XX)
#undef XX
	ATTRIBUTE_COUNT
};

struct attrs
{
	short fg;                           ///< Foreground colour index
	short bg;                           ///< Background colour index
	chtype attrs;                       ///< Other attributes
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

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
typedef void (*tab_item_draw_fn)
	(struct tab *self, unsigned item_index, struct row_buffer *buffer);

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
	struct str_map song_info;           ///< Current song info

	struct poller_timer elapsed_event;  ///< Seconds elapsed event
	// TODO: initialize these to -1
	int song_elapsed;                   ///< Song elapsed in seconds
	int song_duration;                  ///< Song duration in seconds
	int volume;                         ///< Current volume

	// Data:

	struct config config;               ///< Program configuration

	struct tab *tabs;                   ///< All tabs
	struct tab *active_tab;             ///< Active tab

	// Emulated widgets:

	int top_height;                     ///< Height of the top part

	int controls_offset;                ///< Offset to player controls or -1
	int gauge_offset;                   ///< Offset to the gauge or -1
	int gauge_width;                    ///< Width of the gauge, if present

	// Terminal:

	termo_t *tk;                        ///< termo handle
	struct poller_timer tk_timer;       ///< termo timeout timer
	bool locale_is_utf8;                ///< The locale is Unicode

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
	str_map_free (&g_ctx.song_info);

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
	LIST_HEADER (struct row_char)

	ucs4_t c;                           ///< Unicode codepoint
	chtype attrs;                       ///< Special attributes
	int width;                          ///< How many cells this takes
};

struct row_buffer
{
	struct row_char *chars;             ///< Characters
	struct row_char *chars_tail;        ///< Tail of characters
	size_t chars_len;                   ///< Character count
	int total_width;                    ///< Total width of all characters
};

static void
row_buffer_init (struct row_buffer *self)
{
	memset (self, 0, sizeof *self);
}

static void
row_buffer_free (struct row_buffer *self)
{
	LIST_FOR_EACH (struct row_char, it, self->chars)
		free (it);
}

/// Replace invalid chars and push all codepoints to the array w/ attributes.
static void
row_buffer_append (struct row_buffer *self, const char *str, chtype attrs)
{
	// The encoding is only really used internally for some corner cases
	const char *encoding = locale_charset ();

	ucs4_t c;
	const uint8_t *start = (const uint8_t *) str, *next = start;
	while ((next = u8_next (&c, next)))
	{
		if (uc_width (c, encoding) < 0
		 || !app_is_character_in_locale (c))
			c = '?';

		struct row_char *rc = xmalloc (sizeof *rc);
		*rc = (struct row_char)
			{ .c = c, .attrs = attrs, .width = uc_width (c, encoding) };
		LIST_APPEND_WITH_TAIL (self->chars, self->chars_tail, rc);
		self->chars_len++;
		self->total_width += rc->width;
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
	while (self->chars && made < space)
	{
		struct row_char *tail = self->chars_tail;
		LIST_UNLINK_WITH_TAIL (self->chars, self->chars_tail, tail);
		self->chars_len--;
		made += tail->width;
		free (tail);
	}
	self->total_width -= made;
	return made;
}

static void
row_buffer_ellipsis (struct row_buffer *self, int target, chtype attrs)
{
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
	if (!self->chars)
		return;

	// We only NUL-terminate the chunks because of the libunistring API
	uint32_t chunk[self->chars_len + 1], *insertion_point = chunk;
	LIST_FOR_EACH (struct row_char, it, self->chars)
	{
		if (it->prev && it->attrs != it->prev->attrs)
		{
			row_buffer_print (chunk, it->prev->attrs);
			insertion_point = chunk;
		}
		*insertion_point++ = it->c;
		*insertion_point = 0;
	}
	row_buffer_print (chunk, self->chars_tail->attrs);
}

// --- Help tab ----------------------------------------------------------------

// TODO: either find something else to put in here or remove the wrapper struct
static struct
{
	struct tab super;                   ///< Parent class
}
g_help_tab;

static struct help_tab_item
{
	const char *text;                   ///< Item text
}
g_help_items[] =
{
	{ "First entry on the list" },
	{ "Something different" },
	{ "Yet another item" },
};

static void
help_tab_on_item_draw (struct tab *self, unsigned item_index,
	struct row_buffer *buffer)
{
	(void) self;

	hard_assert (item_index <= N_ELEMENTS (g_help_items));
	row_buffer_append (buffer, g_help_items[item_index].text, 0);
}

static struct tab *
help_tab_create ()
{
	struct tab *super = &g_help_tab.super;
	tab_init (super, "Help");
	super->on_item_draw = help_tab_on_item_draw;
	super->item_count = N_ELEMENTS (g_help_items);
	super->item_selected = 0;
	return super;
}

// --- Rendering ---------------------------------------------------------------

/// Write the given UTF-8 string padded with spaces.
/// @param[in] n  The number of characters to write, or -1 for the whole string.
/// @param[in] attrs  Text attributes for the text, without padding.
///                   To change the attributes of all output, use attrset().
/// @return The number of characters output.
static size_t
app_write_utf8 (const char *str, chtype attrs, int n)
{
	if (!n)
		return 0;

	struct row_buffer buf;
	row_buffer_init (&buf);
	row_buffer_append (&buf, str, attrs);

	if (n < 0)
		n = buf.total_width;
	if (buf.total_width > n)
		row_buffer_ellipsis (&buf, n, attrs);

	row_buffer_flush (&buf);
	for (int i = buf.total_width; i < n; i++)
		addch (' ');

	row_buffer_free (&buf);
	return n;
}

/// Clear a row in the header to be used and increment the listview offset
static void
app_next_row (chtype attrs)
{
	mvwhline (stdscr, g_ctx.top_height++, 0, ' ' | attrs, COLS);
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
app_redraw_song_info (void)
{
	// The map doesn't need to be initialized at all, so we need to check
	struct str_map *map = &g_ctx.song_info;
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
	int hours   = minutes / 60; hours   %= 60;

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

	static const char *partials[] = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
	int remainder = len_left % N_ELEMENTS (partials);
	len_left /= N_ELEMENTS (partials);

	// Assuming that if we can show the 1/8 box then we can show them all
	const char *partial = NULL;
	// TODO: cache this setting and make it configurable since it doesn't seem
	//   to work 100% (e.g. incompatible with undelining in urxvt)
	if (!app_is_character_in_locale (L'▏'))
		len_left += remainder >= (int) N_ELEMENTS (partials) / 2;
	else
		partial = partials[remainder];

	int len_right = width - len_left;
	while (len_left-- > 0)
		row_buffer_append (buf, " ", APP_ATTR (ELAPSED));
	if (partial && len_right-- > 0)
		row_buffer_append (buf, partial, APP_ATTR (REMAINS));
	while (len_right-- > 0)
		row_buffer_append (buf, " ", APP_ATTR (REMAINS));
}

static void
app_redraw_status (void)
{
	if (g_ctx.state != PLAYER_STOPPED)
		app_redraw_song_info ();

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
	g_ctx.controls_offset = g_ctx.top_height;
	app_flush_buffer (&buf, a_normal);
}

static void
app_redraw_top (void)
{
	// TODO: when it changes from the previous value, fix the selection
	g_ctx.top_height = 0;

	g_ctx.controls_offset = -1;
	g_ctx.gauge_offset = -1;
	g_ctx.gauge_width = 0;

	attrset (0);
	switch (g_ctx.client.state)
	{
	case MPD_CONNECTED:
		app_redraw_status ();
		break;
	case MPD_CONNECTING:
		app_next_row (APP_ATTR (HEADER));
		app_write_utf8 ("Connecting to MPD...", APP_ATTR (HEADER), COLS);
		break;
	case MPD_DISCONNECTED:
		app_next_row (APP_ATTR (HEADER));
		app_write_utf8 ("Disconnected", APP_ATTR (HEADER), COLS);
	}

	attrset (APP_ATTR (TAB_BAR));
	app_next_row (0);
	// TODO: render this with APP_ATTR (TAB_ACTIVE) when the help tab is selected;
	//   ...maybe the help tab should not even be on the list?
	size_t indent = app_write_utf8 (APP_TITLE, 0, -1);

	addch (' ');
	indent++;

	attrset (0);
	LIST_FOR_EACH (struct tab, it, g_ctx.tabs)
	{
		indent += app_write_utf8 (it->name,
			it == g_ctx.active_tab ? APP_ATTR (TAB_ACTIVE) : APP_ATTR (TAB_BAR),
			MIN (COLS - indent, it->name_width));
	}
	refresh ();
}

static void
app_redraw_view (void)
{
	move (g_ctx.top_height, 0);
	clrtobot ();

	// TODO: display a scrollbar on the right side
	struct tab *tab = g_ctx.active_tab;
	int to_show = MIN (LINES - g_ctx.top_height,
		(int) tab->item_count - tab->item_top);
	for (int row_index = 0; row_index < to_show; row_index++)
	{
		int item_index = tab->item_top + row_index;
		int row_attrs = (item_index & 1) ? APP_ATTR (ODD) : APP_ATTR (EVEN);
		if (item_index == tab->item_selected)
			row_attrs = APP_ATTR (SELECTION);

		attrset (row_attrs);

		struct row_buffer buf;
		row_buffer_init (&buf);

		tab->on_item_draw (tab, item_index, &buf);
		if (buf.total_width > COLS)
			row_buffer_ellipsis (&buf, COLS, row_attrs);

		row_buffer_flush (&buf);
		for (int i = buf.total_width; i < COLS; i++)
			addch (' ');
		row_buffer_free (&buf);
	}

	attrset (0);
	refresh ();
}

static void
app_redraw (void)
{
	app_redraw_top ();
	app_redraw_view ();
}

// --- Actions -----------------------------------------------------------------

/// Scroll up @a n items.  Doesn't redraw.
static bool
app_scroll_up (int n)
{
	struct tab *tab = g_ctx.active_tab;
	if (tab->item_top < n)
	{
		tab->item_top = 0;
		return false;
	}
	tab->item_top -= n;
	return true;
}

/// Scroll down @a n items.  Doesn't redraw.
static bool
app_scroll_down (int n)
{
	struct tab *tab = g_ctx.active_tab;
	// TODO: if (n_items >= lines), don't allow to scroll off past the end
	if ((tab->item_top += n) >= (int) tab->item_count)
	{
		if (tab->item_count)
			tab->item_top = tab->item_count - 1;
		else
			tab->item_top = 0;
		return false;
	}
	return true;
}

/// Moves the selection one item up.
static bool
app_one_item_up (void)
{
	struct tab *tab = g_ctx.active_tab;
	if (tab->item_selected < 1)
		return false;

	if (--tab->item_selected < tab->item_top)
		app_scroll_up (tab->item_top - tab->item_selected);

	app_redraw_view ();
	return true;
}

/// Moves the selection one item down.
static bool
app_one_item_down (void)
{
	struct tab *tab = g_ctx.active_tab;
	if (tab->item_selected + 1 >= (int) tab->item_count)
		return false;

	int n_visible = LINES - g_ctx.top_height;
	if (++tab->item_selected >= tab->item_top + n_visible)
		app_scroll_down (1);

	app_redraw_view ();
	return true;
}

static bool
app_goto_tab (unsigned n)
{
	// TODO: go to tab n, return false if out of range
	return false;

	app_redraw ();
	return true;
}

static void
app_process_resize (void)
{
	struct tab *tab = g_ctx.active_tab;
	if (tab->item_selected < 0)
		return;

	int n_visible = LINES - g_ctx.top_height;
	if (n_visible < 0)
		return;

	// Scroll up as needed to keep the selection visible
	int selected_offset = tab->item_selected - tab->item_top;
	if (selected_offset >= n_visible)
		app_scroll_up (selected_offset - n_visible + 1);

	app_redraw ();
}

// --- User input handling -----------------------------------------------------

enum user_action
{
	USER_ACTION_NONE,

	USER_ACTION_QUIT,
	USER_ACTION_REDRAW,

	USER_ACTION_MPD_PREVIOUS,
	USER_ACTION_MPD_TOGGLE,
	USER_ACTION_MPD_STOP,
	USER_ACTION_MPD_NEXT,

	USER_ACTION_GOTO_ITEM_PREVIOUS,
	USER_ACTION_GOTO_ITEM_NEXT,
	USER_ACTION_GOTO_PAGE_PREVIOUS,
	USER_ACTION_GOTO_PAGE_NEXT,

	USER_ACTION_COUNT
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
	switch (action)
	{
	case USER_ACTION_QUIT:
		return false;
	case USER_ACTION_REDRAW:
		clear ();
		app_redraw ();
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

	// TODO: relative seeks
#if 0
		MPD_SIMPLE (forward,  "seekcur", "+10", NULL)
		MPD_SIMPLE (backward, "seekcur", "-10", NULL)
#endif

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_GOTO_ITEM_PREVIOUS:
		app_one_item_up ();
		return true;
	case USER_ACTION_GOTO_ITEM_NEXT:
		app_one_item_down ();
		return true;

	case USER_ACTION_GOTO_PAGE_PREVIOUS:
		app_scroll_up (LINES - (int) g_ctx.top_height);
		app_redraw_view ();
		return true;
	case USER_ACTION_GOTO_PAGE_NEXT:
		app_scroll_down (LINES - (int) g_ctx.top_height);
		app_redraw_view ();
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

static bool
app_process_keysym (termo_key_t *event)
{
	enum user_action action = USER_ACTION_NONE;
	typedef const enum user_action ActionMap[TERMO_N_SYMS];

	static ActionMap actions =
	{
		[TERMO_SYM_ESCAPE]    = USER_ACTION_QUIT,

		[TERMO_SYM_UP]        = USER_ACTION_GOTO_ITEM_PREVIOUS,
		[TERMO_SYM_DOWN]      = USER_ACTION_GOTO_ITEM_NEXT,
		[TERMO_SYM_PAGEUP]    = USER_ACTION_GOTO_PAGE_PREVIOUS,
		[TERMO_SYM_PAGEDOWN]  = USER_ACTION_GOTO_PAGE_NEXT,
	};
	static ActionMap actions_alt =
	{
	};
	static ActionMap actions_ctrl =
	{
	};

	if (!event->modifiers)
		action = actions[event->code.sym];
	else if (event->modifiers == TERMO_KEYMOD_ALT)
		action = actions_alt[event->code.sym];
	else if (event->modifiers == TERMO_KEYMOD_CTRL)
		action = actions_ctrl[event->code.sym];

	return app_process_user_action (action);
}

static bool
app_process_ctrl_key (termo_key_t *event)
{
	static const enum user_action actions[32] =
	{
		[CTRL_KEY ('L')]      = USER_ACTION_REDRAW,

		[CTRL_KEY ('P')]      = USER_ACTION_GOTO_ITEM_PREVIOUS,
		[CTRL_KEY ('N')]      = USER_ACTION_GOTO_ITEM_NEXT,
		[CTRL_KEY ('B')]      = USER_ACTION_GOTO_PAGE_PREVIOUS,
		[CTRL_KEY ('F')]      = USER_ACTION_GOTO_PAGE_NEXT,
	};

	int64_t i = (int64_t) event->code.codepoint - 'a' + 1;
	if (i > 0 && i < (int64_t) N_ELEMENTS (actions))
		return app_process_user_action (actions[i]);

	return true;
}

static bool
app_process_alt_key (termo_key_t *event)
{
	if (event->code.codepoint >= '0'
	 && event->code.codepoint <= '9')
	{
		int n = event->code.codepoint - '0';
		if (!app_goto_tab ((n == 0 ? 10 : n) - 1))
			beep ();
	}
	return true;
}

static bool
app_process_key (termo_key_t *event)
{
	if (event->modifiers == TERMO_KEYMOD_CTRL)
		return app_process_ctrl_key (event);
	if (event->modifiers == TERMO_KEYMOD_ALT)
		return app_process_alt_key (event);
	if (event->modifiers)
		return true;

	// TODO: normal unmodified keys will have functions as well
	ucs4_t c = event->code.codepoint;
	return true;
}

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
	else if (line == g_ctx.top_height - 1)
	{
		struct tab *winner = NULL;
		int indent = strlen (APP_TITLE);
		// TODO: set the winner to the special help tab in this case
		if (column < indent)
			return;
		for (struct tab *iter = g_ctx.tabs; !winner && iter; iter = iter->next)
		{
			if (column < (indent += iter->name_width))
				winner = iter;
		}
		if (winner)
		{
			g_ctx.active_tab = winner;
			app_redraw ();
		}
	}
	else
	{
		struct tab *tab = g_ctx.active_tab;
		int row_index = line - g_ctx.top_height;
		if (row_index >= (int) tab->item_count - tab->item_top)
			return;

		tab->item_selected = row_index + tab->item_top;
		app_redraw_view ();
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
		app_process_user_action (USER_ACTION_GOTO_ITEM_PREVIOUS);
	else if (button == 5)
		app_process_user_action (USER_ACTION_GOTO_ITEM_NEXT);

	return true;
}

static bool
app_process_termo_event (termo_key_t *event)
{
	switch (event->type)
	{
	case TERMO_TYPE_MOUSE:
		return app_process_mouse (event);
	case TERMO_TYPE_KEY:
		return app_process_key (event);
	case TERMO_TYPE_KEYSYM:
		return app_process_keysym (event);
	default:
		return true;
	}
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

// --- MPD interface -----------------------------------------------------------

// TODO: this entire thing has been slavishly copy-pasted from dwmstatus
// TODO: try to move some of this code to mpd.c

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
	str_map_free (&g_ctx.song_info);
	poller_timer_reset (&g_ctx.elapsed_event);

	if (!response->success)
	{
		print_debug ("%s: %s",
			"retrieving MPD info failed", response->message_text);
		return;
	}

	struct str_map map;
	mpd_vector_to_map (data, &map);

	const char *value;
	g_ctx.state = PLAYER_PLAYING;
	if ((value = str_map_find (&map, "state")))
	{
		if (!strcmp (value, "stop"))
			g_ctx.state = PLAYER_STOPPED;
		if (!strcmp (value, "pause"))
			g_ctx.state = PLAYER_PAUSED;
	}

	// Note that we may receive a "time" field twice, however the right one
	// wins here due to the order we send the commands in
	char *time     = str_map_find (&map, "time");
	char *duration = str_map_find (&map, "duration");
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

	// TODO: use "time" as a fallback (no milliseconds there)
	char *elapsed = str_map_find (&map, "elapsed");
	if (elapsed && g_ctx.state == PLAYER_PLAYING)
	{
		// TODO: parse the "elapsed" value and use it
		char *period = strchr (elapsed, '.');
		if (period && xstrtoul (&tmp, period + 1, 10))
		{
			// TODO: initialize the timer and create a callback
			poller_timer_set (&g_ctx.elapsed_event, 1000 - tmp);
		}
	}

	char *volume = str_map_find (&map, "volume");
	if (volume && xstrtoul (&tmp, volume, 10))
		g_ctx.volume = tmp;

	g_ctx.song_info = map;
	app_redraw ();
}

static void
mpd_on_tick (void *user_data)
{
	(void) user_data;
	// FIXME: this is doomed to drift unless we use POSIX CLOCK_MONOTONIC
	poller_timer_set (&g_ctx.elapsed_event, 1000);

	g_ctx.song_elapsed++;
	// TODO: try to be more efficient in the redrawing procedures
	app_redraw ();
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

	if (subsystems & (MPD_SUBSYSTEM_PLAYER | MPD_SUBSYSTEM_PLAYLIST))
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
app_on_reconnect (void *user_data)
{
	(void) user_data;

	struct mpd_client *c = &g_ctx.client;
	c->on_failure   = mpd_on_failure;
	c->on_connected = mpd_on_connected;
	c->on_event     = mpd_on_events;

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
		app_process_resize ();
		g_winch_received = false;
	}
}

static void
app_log_handler (void *user_data, const char *quote, const char *fmt,
	va_list ap)
{
	// TODO: we might want to make use of the user_data (attribute?)
	(void) user_data;

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
	if (isatty (STDERR_FILENO))
	{
		// TODO: remember the position and attributes and restore them
		attrset (A_REVERSE);
		mvwhline (stdscr, LINES - 1, 0, A_REVERSE, COLS);
		app_write_utf8 (message.str, 0, COLS);
	}
	else
		fprintf (stderr, "%s\n", message.str);
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
	g_log_message_real = app_log_handler;

	// TODO: create more tabs
	// TODO: in debug mode add a tab with all messages
	LIST_PREPEND (g_ctx.tabs, help_tab_create ());
	g_ctx.active_tab = g_ctx.tabs;
	app_redraw ();

	signals_setup_handlers ();
	app_init_poller_events ();

	g_ctx.polling = true;
	while (g_ctx.polling)
		poller_run (&g_ctx.poller);

	endwin ();
	g_log_message_real = log_message_stdio;
	app_free_context ();
	return 0;
}

