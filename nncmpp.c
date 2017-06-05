/*
 * nncmpp -- the MPD client you never knew you needed
 *
 * Copyright (c) 2016 - 2017, Přemysl Janouch <p.janouch@gmail.com>
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
	XX( NORMAL,     "normal",     -1, -1, 0           ) \
	XX( HIGHLIGHT,  "highlight",  -1, -1, A_BOLD      ) \
	/* Gauge                                         */ \
	XX( ELAPSED,    "elapsed",    -1, -1, A_REVERSE   ) \
	XX( REMAINS,    "remains",    -1, -1, A_UNDERLINE ) \
	/* Tab bar                                       */ \
	XX( TAB_BAR,    "tab_bar",    -1, -1, A_REVERSE   ) \
	XX( TAB_ACTIVE, "tab_active", -1, -1, A_UNDERLINE ) \
	/* Listview                                      */ \
	XX( HEADER,     "header",     -1, -1, A_UNDERLINE ) \
	XX( EVEN,       "even",       -1, -1, 0           ) \
	XX( ODD,        "odd",        -1, -1, 0           ) \
	XX( DIRECTORY,  "directory",  -1, -1, 0           ) \
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
#define LIBERTY_WANT_PROTO_HTTP
#define LIBERTY_WANT_PROTO_MPD
#include "liberty/liberty.c"
#include "liberty/liberty-tui.c"

#include <locale.h>
#include <termios.h>
#ifndef TIOCGWINSZ
#include <sys/ioctl.h>
#endif  // ! TIOCGWINSZ

// ncurses is notoriously retarded for input handling, we need something
// different if only to receive mouse events reliably.

#include "termo.h"

// We need cURL to extract links from Internet stream playlists.  It'd be way
// too much code to do this all by ourselves, and there's nothing better around.

#include <curl/curl.h>

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

static bool
xstrtoul_map (const struct str_map *map, const char *key, unsigned long *out)
{
	const char *field = str_map_find (map, key);
	return field && xstrtoul (out, field, 10);
}

static const char *
xbasename (const char *path)
{
	const char *last_slash = strrchr (path, '/');
	return last_slash ? last_slash + 1 : path;
}

static char *
latin1_to_utf8 (const char *latin1)
{
	struct str converted;
	str_init (&converted);
	while (*latin1)
	{
		uint8_t c = *latin1++;
		if (c < 0x80)
			str_append_c (&converted, c);
		else
		{
			str_append_c (&converted, 0xC0 | (c >> 6));
			str_append_c (&converted, 0x80 | (c & 0x3F));
		}
	}
	return str_steal (&converted);
}

static void
cstr_uncapitalize (char *s)
{
	if (isupper (s[0]) && islower (s[1]))
		s[0] = tolower_ascii (s[0]);
}

static int
print_curl_debug (CURL *easy, curl_infotype type, char *data, size_t len,
	void *ud)
{
	(void) easy;
	(void) ud;
	(void) type;

	char copy[len + 1];
	for (size_t i = 0; i < len; i++)
	{
		uint8_t c = data[i];
		copy[i] = c >= 32 || c == '\n' ? c : '.';
	}
	copy[len] = '\0';

	char *next;
	for (char *p = copy; p; p = next)
	{
		if ((next = strchr (p, '\n')))
			*next++ = '\0';
		if (!*p)
			continue;

		if (!utf8_validate (p, strlen (p)))
		{
			char *fixed = latin1_to_utf8 (p);
			print_debug ("cURL: %s", fixed);
			free (fixed);
		}
		else
			print_debug ("cURL: %s", p);
	}
	return 0;
}

static char *
mpd_parse_kv (char *line, char **value)
{
	char *key = mpd_client_parse_kv (line, value);
	if (!key)  print_debug ("%s: %s", "erroneous MPD output", line);
	return key;
}

// --- cURL async wrapper ------------------------------------------------------

// You are meant to subclass this structure, no user_data pointers needed
struct poller_curl_task;

/// Receives notification for finished transfers
typedef void (*poller_curl_done_fn)
	(CURLMsg *msg, struct poller_curl_task *task);

struct poller_curl_task
{
	CURL *easy;                         ///< cURL easy interface handle
	char curl_error[CURL_ERROR_SIZE];   ///< cURL error info buffer
	poller_curl_done_fn on_done;        ///< Done callback
};

struct poller_curl_fd
{
	LIST_HEADER (struct poller_curl_fd)
	struct poller_fd fd;                ///< Poller FD
};

struct poller_curl
{
	struct poller *poller;              ///< Parent poller
	struct poller_timer timer;          ///< cURL timer
	CURLM *multi;                       ///< cURL multi interface handle
	struct poller_curl_fd *fds;         ///< List of all FDs
};

static void
poller_curl_collect (struct poller_curl *self, curl_socket_t s, int ev_bitmask)
{
	int running = 0;
	CURLMcode res;
	// XXX: ignoring errors, in particular CURLM_CALL_MULTI_PERFORM
	if ((res = curl_multi_socket_action (self->multi, s, ev_bitmask, &running)))
		print_debug ("cURL: %s", curl_multi_strerror (res));

	CURLMsg *msg;
	while ((msg = curl_multi_info_read (self->multi, &running)))
		if (msg->msg == CURLMSG_DONE)
		{
			struct poller_curl_task *task = NULL;
			hard_assert (!curl_easy_getinfo
				(msg->easy_handle, CURLINFO_PRIVATE, &task));
			task->on_done (msg, task);
		}
}

static void
poller_curl_on_socket (const struct pollfd *pfd, void *user_data)
{
	int mask = 0;
	if (pfd->revents & POLLIN)  mask |= CURL_CSELECT_IN;
	if (pfd->revents & POLLOUT) mask |= CURL_CSELECT_OUT;
	if (pfd->revents & POLLERR) mask |= CURL_CSELECT_ERR;
	poller_curl_collect (user_data, pfd->fd, mask);
}

static int
poller_curl_on_socket_action (CURL *easy, curl_socket_t s, int what,
	void *user_data, void *socket_data)
{
	(void) easy;
	struct poller_curl *self = user_data;

	struct poller_curl_fd *fd;
	if (!(fd = socket_data))
	{
		fd = xmalloc (sizeof *fd);
		LIST_PREPEND (self->fds, fd);

		poller_fd_init (&fd->fd, self->poller, s);
		fd->fd.dispatcher = poller_curl_on_socket;
		fd->fd.user_data = self;
		curl_multi_assign (self->multi, s, fd);
	}
	if (what == CURL_POLL_REMOVE)
	{
		poller_fd_reset (&fd->fd);
		LIST_UNLINK (self->fds, fd);
		free (fd);
	}
	else
	{
		short events = 0;
		if (what == CURL_POLL_IN)    events = POLLIN;
		if (what == CURL_POLL_OUT)   events =          POLLOUT;
		if (what == CURL_POLL_INOUT) events = POLLIN | POLLOUT;
		poller_fd_set (&fd->fd, events);
	}
	return 0;
}

static void
poller_curl_on_timer (void *user_data)
{
	poller_curl_collect (user_data, CURL_SOCKET_TIMEOUT, 0);
}

static int
poller_curl_on_timer_change (CURLM *multi, long timeout_ms, void *user_data)
{
	(void) multi;
	struct poller_curl *self = user_data;

	if (timeout_ms < 0)
		poller_timer_reset (&self->timer);
	else
		poller_timer_set (&self->timer, timeout_ms);
	return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
poller_curl_init (struct poller_curl *self, struct poller *poller,
	struct error **e)
{
	memset (self, 0, sizeof *self);
	if (!(self->multi = curl_multi_init ()))
		return error_set (e, "cURL setup failed");

	CURLMcode mres;
	if ((mres = curl_multi_setopt (self->multi,
			CURLMOPT_SOCKETFUNCTION, poller_curl_on_socket_action))
	 || (mres = curl_multi_setopt (self->multi,
			CURLMOPT_TIMERFUNCTION, poller_curl_on_timer_change))
	 || (mres = curl_multi_setopt (self->multi, CURLMOPT_SOCKETDATA, self))
	 || (mres = curl_multi_setopt (self->multi, CURLMOPT_TIMERDATA, self)))
	{
		curl_multi_cleanup (self->multi);
		return error_set (e, "%s: %s",
			"cURL setup failed", curl_multi_strerror (mres));
	}

	poller_timer_init (&self->timer, (self->poller = poller));
	self->timer.dispatcher = poller_curl_on_timer;
	self->timer.user_data = self;
	return true;
}

static void
poller_curl_free (struct poller_curl *self)
{
	curl_multi_cleanup (self->multi);
	poller_timer_reset (&self->timer);

	LIST_FOR_EACH (struct poller_curl_fd, iter, self->fds)
	{
		poller_fd_reset (&iter->fd);
		free (iter);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Initialize a task with a new easy instance that can be used with the poller
static bool
poller_curl_spawn (struct poller_curl_task *task, struct error **e)
{
	CURL *easy;
	if (!(easy = curl_easy_init ()))
		return error_set (e, "cURL setup failed");

	// We already take care of SIGPIPE, and native DNS timeouts are only
	// a problem for people without the AsynchDNS feature.
	//
	// Unfortunately, cURL doesn't allow custom callbacks for DNS.
	// The most we could try is parse out the hostname and provide an address
	// override for it using CURLOPT_RESOLVE.  Or be our own SOCKS4A/5 proxy.

	CURLcode res;
	if ((res = curl_easy_setopt (easy, CURLOPT_NOSIGNAL,    1L))
	 || (res = curl_easy_setopt (easy, CURLOPT_ERRORBUFFER, task->curl_error))
	 || (res = curl_easy_setopt (easy, CURLOPT_PRIVATE,     task)))
	{
		curl_easy_cleanup (easy);
		return error_set (e, "%s", curl_easy_strerror (res));
	}

	task->easy = easy;
	return true;
}

static bool
poller_curl_add (struct poller_curl *self, CURL *easy, struct error **e)
{
	CURLMcode mres;
	// "CURLMOPT_TIMERFUNCTION [...] will be called from within this function"
	if ((mres = curl_multi_add_handle (self->multi, easy)))
		return error_set (e, "%s", curl_multi_strerror (mres));
	return true;
}

static bool
poller_curl_remove (struct poller_curl *self, CURL *easy, struct error **e)
{
	CURLMcode mres;
	if ((mres = curl_multi_remove_handle (self->multi, easy)))
		return error_set (e, "%s", curl_multi_strerror (mres));
	return true;
}

// --- Compact map -------------------------------------------------------------

// MPD provides us with a hefty amount of little key-value maps.  The overhead
// of str_map for such constant (string -> string) maps is too high and it's
// much better to serialize them (mainly cache locality and memory efficiency).
//
// This isn't intended to be reusable and has case insensitivity built-in.

typedef uint8_t *compact_map_t;         ///< Compacted (string -> string) map

static compact_map_t
compact_map (struct str_map *map)
{
	struct str s;
	str_init (&s);
	struct str_map_iter iter;
	str_map_iter_init (&iter, map);

	char *value;
	static const size_t zero = 0, alignment = sizeof zero;
	while ((value = str_map_iter_next (&iter)))
	{
		size_t entry_len = iter.link->key_length + 1 + strlen (value) + 1;
		size_t padding_len = (alignment - entry_len % alignment) % alignment;
		entry_len += padding_len;

		str_append_data (&s, &entry_len, sizeof entry_len);
		str_append_printf (&s, "%s%c%s%c", iter.link->key, 0, value, 0);
		str_append_data (&s, &zero, padding_len);
	}
	str_append_data (&s, &zero, sizeof zero);
	return (compact_map_t) str_steal (&s);
}

static char *
compact_map_find (compact_map_t data, const char *needle)
{
	size_t entry_len;
	while ((entry_len = *(size_t *) data))
	{
		data += sizeof entry_len;
		if (!strcasecmp_ascii (needle, (const char *) data))
			return (char *) data + strlen (needle) + 1;
		data += entry_len;
	}
	return NULL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct item_list
{
	compact_map_t *items;               ///< Compacted (string -> string) maps
	size_t len;                         ///< Length
	size_t alloc;                       ///< Allocated items
};

static void
item_list_init (struct item_list *self)
{
	memset (self, 0, sizeof *self);
	self->items = xcalloc (sizeof *self->items, (self->alloc = 16));
}

static void
item_list_free (struct item_list *self)
{
	for (size_t i = 0; i < self->len; i++)
		free (self->items[i]);
	free (self->items);
}

static bool
item_list_set (struct item_list *self, int i, struct str_map *item)
{
	if (i < 0 || (size_t) i >= self->len)
		return false;

	free (self->items[i]);
	self->items[i] = compact_map (item);
	return true;
}

static compact_map_t
item_list_get (struct item_list *self, int i)
{
	if (i < 0 || (size_t) i >= self->len || !self->items[i])
		return NULL;
	return self->items[i];
}

static void
item_list_resize (struct item_list *self, size_t len)
{
	// Make the allocated array big enough but not too large
	size_t new_alloc = self->alloc;
	while (new_alloc < len)
		new_alloc <<= 1;
	while ((new_alloc >> 1) >= len
		&& (new_alloc - len) >= 1024)
		new_alloc >>= 1;

	for (size_t i = len; i < self->len; i++)
		free (self->items[i]);
	if (new_alloc != self->alloc)
		self->items = xreallocarray (self->items,
			sizeof *self->items, (self->alloc = new_alloc));
	for (size_t i = self->len; i < len; i++)
		self->items[i] = NULL;

	self->len = len;
}

// --- Application -------------------------------------------------------------

// Function names are prefixed mostly because of curses which clutters the
// global namespace and makes it harder to distinguish what functions relate to.

// The user interface is focused on conceptual simplicity.  That is important
// since we're not using any TUI framework (which are mostly a lost cause to me
// in the post-Unicode era and not worth pursuing), and the code would get
// bloated and incomprehensible fast.  We mostly rely on "row_buffer" to write
// text from left to right row after row while keeping track of cells.
//
// There is an independent top pane displaying general status information,
// followed by a tab bar and a listview served by a per-tab event handler.
//
// For simplicity, the listview can only work with items that are one row high.

struct tab;
struct row_buffer;
enum action;

/// Try to handle an action in the tab
typedef bool (*tab_action_fn) (enum action action);

/// Draw an item to the screen using the row buffer API
typedef void (*tab_item_draw_fn)
	(size_t item_index, struct row_buffer *buffer, int width);

struct tab
{
	LIST_HEADER (struct tab)

	char *name;                         ///< Visible identifier
	size_t name_width;                  ///< Visible width of the name

	char *header;                       ///< The header, should there be any

	// Implementation:

	tab_action_fn on_action;            ///< User action handler callback
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

	struct poller_timer message_timer;  ///< Message timeout
	char *message;                      ///< Message to show in the statusbar

	// Connection:

	struct mpd_client client;           ///< MPD client interface
	struct poller_timer connect_event;  ///< MPD reconnect timer

	enum player_state state;            ///< Player state
	struct str_map playback_info;       ///< Current song info

	struct poller_timer elapsed_event;  ///< Seconds elapsed event
	int64_t elapsed_since;              ///< Time of the next tick

	// TODO: initialize these to -1
	int song;                           ///< Current song index
	int song_elapsed;                   ///< Song elapsed in seconds
	int song_duration;                  ///< Song duration in seconds
	int volume;                         ///< Current volume

	struct item_list playlist;          ///< Current playlist
	uint32_t playlist_version;          ///< Playlist version
	int playlist_time;                  ///< Play time in seconds

	// Data:

	struct config config;               ///< Program configuration
	struct strv streams;                ///< List of "name NUL URI NUL"

	struct tab *help_tab;               ///< Special help tab
	struct tab *tabs;                   ///< All other tabs
	struct tab *active_tab;             ///< Active tab
	struct tab *last_tab;               ///< Previous tab

	// Emulated widgets:

	int header_height;                  ///< Height of the header

	int tabs_offset;                    ///< Offset to tabs or -1
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
g;

/// Shortcut to retrieve named terminal attributes
#define APP_ATTR(name) g.attrs[ATTRIBUTE_ ## name].attrs

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
	self->item_selected = 0;
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
	  .default_  = "\"localhost\"" },
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
	const char *value;
#define XX(name, config, fg_, bg_, attrs_) \
	if ((value = get_config_string (subtree, config))) \
		g.attrs[ATTRIBUTE_ ## name] = attrs_decode (value);
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static int
app_casecmp (const uint8_t *a, const uint8_t *b)
{
	int res;
	// XXX: this seems to produce some strange results
	if (u8_casecmp (a, strlen ((const char *) a), b, strlen ((const char *) b),
		NULL, NULL, &res))
		res = u8_strcmp (a, b);
	return res;
}

static int
strv_sort_utf8_cb (const void *a, const void *b)
{
	return app_casecmp (*(const uint8_t **) a, *(const uint8_t **) b);
}

static void
load_config_streams (struct config_item *subtree, void *user_data)
{
	(void) user_data;

	// XXX: we can't use the tab in load_config_streams() because it hasn't
	//   been initialized yet, and we cannot initialize it before the
	//   configuration has been loaded.  Thus we load it into the app_context.
	struct str_map_iter iter;
	str_map_iter_init (&iter, &subtree->value.object);
	struct config_item *item;
	while ((item = str_map_iter_next (&iter)))
		if (!config_item_type_is_string (item->type))
			print_warning ("`%s': stream URIs must be strings", iter.link->key);
		else
		{
			strv_append_owned (&g.streams, xstrdup_printf ("%s%c%s",
				iter.link->key, 0, item->value.string.str));
		}
	qsort (g.streams.vector, g.streams.len,
		sizeof *g.streams.vector, strv_sort_utf8_cb);
}

static void
app_load_configuration (void)
{
	struct config *config = &g.config;
	config_register_module (config, "settings", load_config_settings, NULL);
	config_register_module (config, "colors",   load_config_colors,   NULL);
	config_register_module (config, "streams",  load_config_streams,  NULL);

	// Bootstrap configuration, so that we can access schema items at all
	config_load (config, config_item_object ());

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
		config_load (&g.config, root);
		config_schema_call_changed (g.config.root);
	}
}

// --- Application -------------------------------------------------------------

static void
app_init_attributes (void)
{
#define XX(name, config, fg_, bg_, attrs_)      \
	g.attrs[ATTRIBUTE_ ## name].fg    = fg_;    \
	g.attrs[ATTRIBUTE_ ## name].bg    = bg_;    \
	g.attrs[ATTRIBUTE_ ## name].attrs = attrs_;
	ATTRIBUTE_TABLE (XX)
#undef XX
}

static void
app_init_context (void)
{
	poller_init (&g.poller);
	mpd_client_init (&g.client, &g.poller);
	config_init (&g.config);
	strv_init (&g.streams);
	item_list_init (&g.playlist);

	str_map_init (&g.playback_info);
	g.playback_info.key_xfrm = tolower_ascii_strxfrm;
	g.playback_info.free = free;

	// This is also approximately what libunistring does internally,
	// since the locale name is canonicalized by locale_charset().
	// Note that non-Unicode locales are handled pretty inefficiently.
	g.locale_is_utf8 = !strcasecmp_ascii (locale_charset (), "UTF-8");

	// It doesn't work 100% (e.g. incompatible with undelining in urxvt)
	// TODO: make this configurable
	g.use_partial_boxes = g.locale_is_utf8;

	app_init_attributes ();
}

static void
app_init_terminal (void)
{
	TERMO_CHECK_VERSION;
	if (!(g.tk = termo_new (STDIN_FILENO, NULL, 0)))
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
		if (g.attrs[a].fg >= COLORS || g.attrs[a].fg < -1
		 || g.attrs[a].bg >= COLORS || g.attrs[a].bg < -1)
		{
			app_init_attributes ();
			return;
		}

		init_pair (a + 1, g.attrs[a].fg, g.attrs[a].bg);
		g.attrs[a].attrs |= COLOR_PAIR (a + 1);
	}
}

static void
app_free_context (void)
{
	mpd_client_free (&g.client);
	str_map_free (&g.playback_info);
	strv_free (&g.streams);
	item_list_free (&g.playlist);

	config_free (&g.config);
	poller_free (&g.poller);
	free (g.message);

	if (g.tk)
		termo_destroy (g.tk);
}

static void
app_quit (void)
{
	g.quitting = true;

	// So far there's nothing for us to wait on, so let's just stop looping;
	// otherwise we might want to e.g. cleanly bring down the MPD interface
	g.polling = false;
}

static bool
app_is_character_in_locale (ucs4_t ch)
{
	// Avoid the overhead joined with calling iconv() for all characters.
	if (g.locale_is_utf8)
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

// --- Rendering ---------------------------------------------------------------

// TODO: rewrite this so that it's fine-grained but not complicated
static void
app_invalidate (void)
{
	poller_idle_set (&g.refresh_event);
}

static void
app_flush_buffer (struct row_buffer *buf, int width, chtype attrs)
{
	row_buffer_align (buf, width, attrs);
	row_buffer_flush (buf);
	row_buffer_free (buf);
}

/// Write the given UTF-8 string padded with spaces.
/// @param[in] attrs  Text attributes for the text, including padding.
static void
app_write_line (const char *str, chtype attrs)
{
	struct row_buffer buf;
	row_buffer_init (&buf);
	row_buffer_append (&buf, str, attrs);
	app_flush_buffer (&buf, COLS, attrs);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
app_flush_header (struct row_buffer *buf, chtype attrs)
{
	move (g.header_height++, 0);
	app_flush_buffer (buf, COLS, attrs);
}

static void
app_draw_song_info (void)
{
	compact_map_t map;
	if (!(map = item_list_get (&g.playlist, g.song)))
		return;

	chtype attr_normal    = APP_ATTR (NORMAL);
	chtype attr_highlight = APP_ATTR (HIGHLIGHT);

	char *title;
	if ((title = compact_map_find (map, "title"))
	 || (title = compact_map_find (map, "name"))
	 || (title = compact_map_find (map, "file")))
	{
		struct row_buffer buf;
		row_buffer_init (&buf);
		row_buffer_append (&buf, title, attr_highlight);
		app_flush_header (&buf, attr_normal);
	}

	char *artist = compact_map_find (map, "artist");
	char *album  = compact_map_find (map, "album");
	if (!artist && !album)
		return;

	struct row_buffer buf;
	row_buffer_init (&buf);

	if (artist)
		row_buffer_append_args (&buf, " by "   + !buf.total_width, attr_normal,
			artist, attr_highlight, NULL);
	if (album)
		row_buffer_append_args (&buf, " from " + !buf.total_width, attr_normal,
			album,  attr_highlight, NULL);
	app_flush_header (&buf, attr_normal);
}

static char *
app_time_string (int seconds)
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
	return str_steal (&s);
}

static void
app_write_time (struct row_buffer *buf, int seconds, chtype attrs)
{
	char *s = app_time_string (seconds);
	row_buffer_append (buf, s, attrs);
	free (s);
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
	if (g.use_partial_boxes)
		partial = partials[remainder];
	else
		len_left += remainder >= (int) 4;

	int len_right = width - len_left;
	row_buffer_space (buf, len_left, APP_ATTR (ELAPSED));
	if (partial && len_right-- > 0)
		row_buffer_append (buf, partial, APP_ATTR (REMAINS));
	row_buffer_space (buf, len_right, APP_ATTR (REMAINS));
}

static void
app_draw_status (void)
{
	if (g.state != PLAYER_STOPPED)
		app_draw_song_info ();

	chtype attr_normal    = APP_ATTR (NORMAL);
	chtype attr_highlight = APP_ATTR (HIGHLIGHT);

	struct row_buffer buf;
	row_buffer_init (&buf);

	bool stopped = g.state == PLAYER_STOPPED;
	chtype attr_song_action = stopped ? attr_normal : attr_highlight;

	const char *toggle = g.state == PLAYER_PLAYING ? "||" : "|>";
	row_buffer_append_args (&buf,
		"<<",   attr_song_action, " ",  attr_normal,
		toggle, attr_highlight,   " ",  attr_normal,
		"[]",   attr_song_action, " ",  attr_normal,
		">>",   attr_song_action, "  ", attr_normal,
		NULL);

	if (stopped)
		row_buffer_append (&buf, "Stopped", attr_normal);
	else
	{
		if (g.song_elapsed >= 0)
		{
			app_write_time (&buf, g.song_elapsed, attr_normal);
			row_buffer_append (&buf, " ", attr_normal);
		}
		if (g.song_duration >= 1)
		{
			row_buffer_append (&buf, "/ ", attr_normal);
			app_write_time (&buf, g.song_duration, attr_normal);
			row_buffer_append (&buf, " ", attr_normal);
		}
		row_buffer_append (&buf, " ", attr_normal);
	}

	// It gets a bit complicated due to the only right-aligned item on the row
	char *volume = NULL;
	int remaining = COLS - buf.total_width;
	if (g.volume >= 0)
	{
		volume = xstrdup_printf ("  %3d%%", g.volume);
		remaining -= strlen (volume);
	}

	if (!stopped && g.song_elapsed >= 0 && g.song_duration >= 1
	 && remaining > 0)
	{
		g.gauge_offset = buf.total_width;
		g.gauge_width = remaining;
		app_write_gauge (&buf,
			(float) g.song_elapsed / g.song_duration, remaining);
	}
	else
		row_buffer_space (&buf, remaining, attr_normal);

	if (volume)
	{
		row_buffer_append (&buf, volume, attr_normal);
		free (volume);
	}
	g.controls_offset = g.header_height;
	app_flush_header (&buf, attr_normal);
}

static void
app_draw_header (void)
{
	g.header_height = 0;

	g.tabs_offset = -1;
	g.controls_offset = -1;
	g.gauge_offset = -1;
	g.gauge_width = 0;

	switch (g.client.state)
	{
	case MPD_CONNECTED:
		app_draw_status ();
		break;
	case MPD_CONNECTING:
		move (g.header_height++, 0);
		app_write_line ("Connecting to MPD...", APP_ATTR (NORMAL));
		break;
	case MPD_DISCONNECTED:
		move (g.header_height++, 0);
		app_write_line ("Disconnected", APP_ATTR (NORMAL));
	}

	chtype attrs[2] = { APP_ATTR (TAB_BAR), APP_ATTR (TAB_ACTIVE) };

	struct row_buffer buf;
	row_buffer_init (&buf);

	// The help tab is disguised so that it's not too intruding
	row_buffer_append (&buf, APP_TITLE, attrs[g.active_tab == g.help_tab]);
	row_buffer_append (&buf, " ", attrs[false]);

	g.tabs_offset = g.header_height;
	LIST_FOR_EACH (struct tab, iter, g.tabs)
		row_buffer_append (&buf, iter->name, attrs[iter == g.active_tab]);
	app_flush_header (&buf, attrs[false]);

	const char *header = g.active_tab->header;
	if (header)
	{
		row_buffer_init (&buf);
		row_buffer_append (&buf, header, APP_ATTR (HEADER));
		app_flush_header (&buf, APP_ATTR (HEADER));
	}
}

static int
app_fitting_items (void)
{
	// The raw number of items that would have fit on the terminal
	return LINES - g.header_height - 1 /* status bar */;
}

static int
app_visible_items (void)
{
	return MAX (0, app_fitting_items ());
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
	struct tab *tab = g.active_tab;
	int visible_items = app_visible_items ();

	if (!g.use_partial_boxes)
	{
		// Apparently here we don't want the 0.5 rounding constant
		int length = (float) visible_items / (int) tab->item_count
			* (visible_items - 1);
		int start  = (float) tab->item_top / (int) tab->item_count
			* (visible_items - 1);

		for (int row = 0; row < visible_items; row++)
		{
			move (g.header_height + row, COLS - 1);
			if (row < start || row > start + length + 1)
				addch (' ' | APP_ATTR (SCROLLBAR));
			else
				addch (' ' | APP_ATTR (SCROLLBAR) | A_REVERSE);
		}
		return;
	}

	// TODO: clamp the values, make sure they follow the right order
	// We subtract half a character from both the top and the bottom, hence -1
	// XXX: I'm not completely sure why we need that 0.5 constant in both
	int length = (double) visible_items / tab->item_count
		* (visible_items - 1) * 8 + 0.5;
	int start  = (double) tab->item_top / tab->item_count
		* (visible_items - 1) * 8 + 0.5;

	// Then we make sure the bar is at least one character high, hence +8
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

		move (g.header_height + row, COLS - 1);

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
	move (g.header_height, 0);
	clrtobot ();

	struct tab *tab = g.active_tab;
	bool want_scrollbar = (int) tab->item_count > app_visible_items ();
	int view_width = COLS - want_scrollbar;

	int to_show =
		MIN (app_fitting_items (), (int) tab->item_count - tab->item_top);
	for (int row = 0; row < to_show; row++)
	{
		int item_index = tab->item_top + row;
		int row_attrs = (item_index & 1) ? APP_ATTR (ODD) : APP_ATTR (EVEN);
		if (item_index == tab->item_selected)
			row_attrs = APP_ATTR (SELECTION);

		struct row_buffer buf;
		row_buffer_init (&buf);
		tab->on_item_draw (item_index, &buf, view_width);

		// Combine attributes used by the handler with the defaults.
		// Avoiding attrset() because of row_buffer_flush().
		for (size_t i = 0; i < buf.chars_len; i++)
		{
			chtype *attrs = &buf.chars[i].attrs;
			if (item_index == tab->item_selected)
				*attrs = (*attrs & ~(A_COLOR | A_REVERSE)) | row_attrs;
			else if ((*attrs & A_COLOR) && (row_attrs & A_COLOR))
				*attrs |= (row_attrs & ~A_COLOR);
			else
				*attrs |=  row_attrs;
		}

		move (g.header_height + row, 0);
		app_flush_buffer (&buf, view_width, row_attrs);
	}

	if (want_scrollbar)
		app_draw_scrollbar ();
}

static void
app_write_mpd_status_playlist (struct row_buffer *buf)
{
	struct str stats;
	str_init (&stats);

	if (g.playlist.len == 1)
		str_append_printf (&stats, "1 song ");
	else
		str_append_printf (&stats, "%zu songs ", g.playlist.len);

	int hours   = g.playlist_time / 3600;
	int minutes = g.playlist_time % 3600 / 60;
	if (hours || minutes)
	{
		str_append_c (&stats, ' ');

		if (hours == 1)
			str_append_printf (&stats, " 1 hour");
		else if (hours)
			str_append_printf (&stats, " %d hours", hours);

		if (minutes == 1)
			str_append_printf (&stats, " 1 minute");
		else if (minutes)
			str_append_printf (&stats, " %d minutes", minutes);
	}
	row_buffer_append (buf, stats.str, APP_ATTR (NORMAL));
	str_free (&stats);
}

static void
app_write_mpd_status (struct row_buffer *buf)
{
	struct str_map *map = &g.playback_info;
	if (str_map_find (map, "updating_db"))
		row_buffer_append (buf, "Updating database...", APP_ATTR (NORMAL));
	else
		app_write_mpd_status_playlist (buf);

	struct row_buffer right;
	row_buffer_init (&right);

	const char *s;
	bool repeat  = (s = str_map_find (map, "repeat"))  && strcmp (s, "0");
	bool random  = (s = str_map_find (map, "random"))  && strcmp (s, "0");
	bool single  = (s = str_map_find (map, "single"))  && strcmp (s, "0");
	bool consume = (s = str_map_find (map, "consume")) && strcmp (s, "0");

	// TODO: remove the conditionals once we make them clickable
	chtype a[2] = { APP_ATTR (NORMAL), APP_ATTR (HIGHLIGHT) };
	if (repeat)  row_buffer_append_args (&right,
		" ", APP_ATTR (NORMAL), "repeat",  a[repeat],  NULL);
	if (random)  row_buffer_append_args (&right,
		" ", APP_ATTR (NORMAL), "random",  a[random],  NULL);
	if (single)  row_buffer_append_args (&right,
		" ", APP_ATTR (NORMAL), "single",  a[single],  NULL);
	if (consume) row_buffer_append_args (&right,
		" ", APP_ATTR (NORMAL), "consume", a[consume], NULL);

	row_buffer_space (buf,
		MAX (0, COLS - buf->total_width - right.total_width),
		APP_ATTR (NORMAL));
	row_buffer_append_buffer (buf, &right);
	row_buffer_free (&right);
}

static void
app_draw_statusbar (void)
{
	struct row_buffer buf;
	row_buffer_init (&buf);

	if (g.message)
		row_buffer_append (&buf, g.message, APP_ATTR (HIGHLIGHT));
	else if (g.client.state == MPD_CONNECTED)
		app_write_mpd_status (&buf);

	move (LINES - 1, 0);
	app_flush_buffer (&buf, COLS, APP_ATTR (NORMAL));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Checks what items are visible and returns if the range was alright
static bool
app_fix_view_range (void)
{
	struct tab *tab = g.active_tab;
	if (tab->item_top < 0)
	{
		tab->item_top = 0;
		return false;
	}

	// If the contents are at least as long as the screen, always fill it
	int max_item_top = (int) tab->item_count - app_visible_items ();
	// But don't let that suggest a negative offset
	max_item_top = MAX (max_item_top, 0);

	if (tab->item_top > max_item_top)
	{
		tab->item_top = max_item_top;
		return false;
	}
	return true;
}

static void
app_on_refresh (void *user_data)
{
	(void) user_data;
	poller_idle_reset (&g.refresh_event);

	app_draw_header ();
	app_fix_view_range();
	app_draw_view ();
	app_draw_statusbar ();

	refresh ();
}

// --- Actions -----------------------------------------------------------------

/// Scroll down (positive) or up (negative) @a n items
static bool
app_scroll (int n)
{
	g.active_tab->item_top += n;
	app_invalidate ();
	return app_fix_view_range ();
}

static void
app_ensure_selection_visible (void)
{
	struct tab *tab = g.active_tab;
	if (tab->item_selected < 0 || !tab->item_count)
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
	struct tab *tab = g.active_tab;
	int fixed = tab->item_selected + diff;
	fixed = MIN (fixed, (int) tab->item_count - 1);
	fixed = MAX (fixed, 0);

	bool result = !diff || tab->item_selected != fixed;
	tab->item_selected = fixed;
	app_invalidate ();

	app_ensure_selection_visible ();
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
app_prepend_tab (struct tab *tab)
{
	LIST_PREPEND (g.tabs, tab);
	app_invalidate ();
}

static void
app_switch_tab (struct tab *tab)
{
	if (tab == g.active_tab)
		return;

	g.last_tab = g.active_tab;
	g.active_tab = tab;
	app_invalidate ();
}

static bool
app_goto_tab (int tab_index)
{
	int i = 0;
	LIST_FOR_EACH (struct tab, iter, g.tabs)
		if (i++ == tab_index)
		{
			app_switch_tab (iter);
			return true;
		}
	return false;
}

// --- User input handling -----------------------------------------------------

#define ACTIONS(XX) \
	XX( NONE,               "Do nothing"              ) \
	\
	XX( QUIT,               "Quit application"        ) \
	XX( REDRAW,             "Redraw screen"           ) \
	XX( HELP_TAB,           "Switch to the help tab"  ) \
	XX( LAST_TAB,           "Switch to previous tab"  ) \
	\
	XX( MPD_TOGGLE,         "Toggle play/pause"       ) \
	XX( MPD_STOP,           "Stop playback"           ) \
	XX( MPD_PREVIOUS,       "Previous song"           ) \
	XX( MPD_NEXT,           "Next song"               ) \
	XX( MPD_BACKWARD,       "Seek backwards"          ) \
	XX( MPD_FORWARD,        "Seek forwards"           ) \
	XX( MPD_UPDATE_DB,      "Update MPD database"     ) \
	XX( MPD_VOLUME_UP,      "Increase volume"         ) \
	XX( MPD_VOLUME_DOWN,    "Decrease volume"         ) \
	\
	XX( MPD_ADD,         "Add song to playlist"       ) \
	XX( MPD_REPLACE,     "Replace playlist with song" ) \
	\
	XX( CHOOSE,             "Choose item"             ) \
	XX( DELETE,             "Delete item"             ) \
	XX( UP,                 "Go up a level"           ) \
	\
	XX( SCROLL_UP,          "Scroll up"               ) \
	XX( SCROLL_DOWN,        "Scroll down"             ) \
	\
	XX( GOTO_TOP,           "Go to the top"           ) \
	XX( GOTO_BOTTOM,        "Go to the bottom"        ) \
	XX( GOTO_ITEM_PREVIOUS, "Go to the previous item" ) \
	XX( GOTO_ITEM_NEXT,     "Go to the next item"     ) \
	XX( GOTO_PAGE_PREVIOUS, "Go to the previous page" ) \
	XX( GOTO_PAGE_NEXT,     "Go to the next page"     ) \
	\
	XX( GOTO_VIEW_TOP,      "Select the top item"     ) \
	XX( GOTO_VIEW_CENTER,   "Select the center item"  ) \
	XX( GOTO_VIEW_BOTTOM,   "Select the bottom item"  )

enum action
{
#define XX(name, description) ACTION_ ## name,
	ACTIONS (XX)
#undef XX
	ACTION_COUNT
};

static struct action_info
{
	const char *name;                   ///< Name for user bindings
	const char *description;            ///< Human-readable description
}
g_actions[] =
{
#define XX(name, description) { #name, description },
	ACTIONS (XX)
#undef XX
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_vsend_command (struct mpd_client *self, va_list ap)
{
	struct strv v;
	strv_init (&v);

	const char *command;
	while ((command = va_arg (ap, const char *)))
		strv_append (&v, command);
	mpd_client_send_commandv (self, v.vector);
	strv_free (&v);
}

/// Send a command to MPD without caring about the response
static bool mpd_client_send_simple (struct mpd_client *self, ...)
	ATTRIBUTE_SENTINEL;

static void
mpd_on_simple_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) data;
	(void) user_data;

	if (!response->success)
		print_error ("%s: %s", "command failed", response->message_text);
}

static bool
mpd_client_send_simple (struct mpd_client *self, ...)
{
	if (self->state != MPD_CONNECTED)
		return false;

	va_list ap;
	va_start (ap, self);
	mpd_client_vsend_command (self, ap);
	va_end (ap);

	mpd_client_add_task (self, mpd_on_simple_response, NULL);
	mpd_client_idle (self, 0);
	return true;
}

#define MPD_SIMPLE(...) \
	mpd_client_send_simple (&g.client, __VA_ARGS__, NULL)

static bool
app_process_action (enum action action)
{
	// First let the tab try to handle this
	struct tab *tab = g.active_tab;
	if (tab->on_action && tab->on_action (action))
		return true;

	switch (action)
	{
	case ACTION_QUIT:
		app_quit ();
		return true;
	case ACTION_REDRAW:
		clear ();
		app_invalidate ();
		return true;
	case ACTION_LAST_TAB:
		if (!g.last_tab)
			return false;
		app_switch_tab (g.last_tab);
		return true;
	case ACTION_HELP_TAB:
		app_switch_tab (g.help_tab);
		return true;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case ACTION_MPD_TOGGLE:
		if (g.state == PLAYER_PLAYING) return MPD_SIMPLE ("pause", "1");
		if (g.state == PLAYER_PAUSED)  return MPD_SIMPLE ("pause", "0");
		return MPD_SIMPLE ("play");
	case ACTION_MPD_STOP:      return MPD_SIMPLE ("stop");
	case ACTION_MPD_PREVIOUS:  return MPD_SIMPLE ("previous");
	case ACTION_MPD_NEXT:      return MPD_SIMPLE ("next");
	case ACTION_MPD_FORWARD:   return MPD_SIMPLE ("seekcur", "+10");
	case ACTION_MPD_BACKWARD:  return MPD_SIMPLE ("seekcur", "-10");
	case ACTION_MPD_UPDATE_DB: return MPD_SIMPLE ("update");
	case ACTION_MPD_VOLUME_UP:
		if (g.volume >= 0)
		{
			char *volume = xstrdup_printf ("%d", MIN (100, g.volume + 10));
			MPD_SIMPLE ("setvol", volume);
			free (volume);
		}
		return true;
	case ACTION_MPD_VOLUME_DOWN:
		if (g.volume >= 0)
		{
			char *volume = xstrdup_printf ("%d", MAX (0,   g.volume - 10));
			MPD_SIMPLE ("setvol", volume);
			free (volume);
		}
		return true;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		// XXX: these should rather be parametrized
	case ACTION_SCROLL_UP:   return app_scroll (-3);
	case ACTION_SCROLL_DOWN: return app_scroll  (3);

	case ACTION_GOTO_TOP:
		if (tab->item_count)
		{
			g.active_tab->item_selected = 0;
			app_ensure_selection_visible ();
			app_invalidate ();
		}
		return true;
	case ACTION_GOTO_BOTTOM:
		if (tab->item_count)
		{
			g.active_tab->item_selected =
				MAX (0, (int) g.active_tab->item_count - 1);
			app_ensure_selection_visible ();
			app_invalidate ();
		}
		return true;

	case ACTION_GOTO_ITEM_PREVIOUS:
		return app_move_selection (-1);
	case ACTION_GOTO_ITEM_NEXT:
		return app_move_selection (1);

	case ACTION_GOTO_PAGE_PREVIOUS:
		app_scroll (-app_visible_items ());
		return app_move_selection (-app_visible_items ());
	case ACTION_GOTO_PAGE_NEXT:
		app_scroll (app_visible_items ());
		return app_move_selection (app_visible_items ());

	case ACTION_GOTO_VIEW_TOP:
		g.active_tab->item_selected = g.active_tab->item_top;
		return app_move_selection (0);
	case ACTION_GOTO_VIEW_CENTER:
		g.active_tab->item_selected = g.active_tab->item_top;
		return app_move_selection (MAX (0, app_visible_items () / 2 - 1));
	case ACTION_GOTO_VIEW_BOTTOM:
		g.active_tab->item_selected = g.active_tab->item_top;
		return app_move_selection (MAX (0, app_visible_items () - 1));

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case ACTION_NONE:
		return true;
	default:
		return false;
	}
	return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
app_process_left_mouse_click (int line, int column, bool double_click)
{
	if (line == g.controls_offset)
	{
		// XXX: there could be a push_widget(buf, text, attrs, handler)
		//   function to help with this but it might not be worth it
		enum action action = ACTION_NONE;
		if (column >= 0 && column <=  1) action = ACTION_MPD_PREVIOUS;
		if (column >= 3 && column <=  4) action = ACTION_MPD_TOGGLE;
		if (column >= 6 && column <=  7) action = ACTION_MPD_STOP;
		if (column >= 9 && column <= 10) action = ACTION_MPD_NEXT;

		if (action)
			return app_process_action (action);

		int gauge_offset = column - g.gauge_offset;
		if (g.gauge_offset < 0
		 || gauge_offset < 0 || gauge_offset >= g.gauge_width)
			return false;

		float position = (float) gauge_offset / g.gauge_width;
		if (g.song_duration >= 1)
		{
			char *where = xstrdup_printf ("%f", position * g.song_duration);
			MPD_SIMPLE ("seekcur", where);
			free (where);
		}
	}
	else if (line == g.tabs_offset)
	{
		struct tab *winner = NULL;
		int indent = strlen (APP_TITLE);
		if (column < indent)
		{
			app_switch_tab (g.help_tab);
			return true;
		}
		for (struct tab *iter = g.tabs; !winner && iter; iter = iter->next)
		{
			if (column < (indent += iter->name_width))
				winner = iter;
		}
		if (!winner)
			return false;

		app_switch_tab (winner);
	}
	else if (line >= g.header_height)
	{
		struct tab *tab = g.active_tab;
		int row_index = line - g.header_height;
		if (row_index < 0
		 || row_index >= (int) tab->item_count - tab->item_top)
			return false;

		// TODO: handle the scrollbar a bit better than this
		int visible_items = app_visible_items ();
		if ((int) tab->item_count > visible_items && column == COLS - 1)
			tab->item_top = (float) row_index / visible_items
				* (int) tab->item_count - visible_items / 2;
		else
			tab->item_selected = row_index + tab->item_top;
		app_invalidate ();

		if (double_click)
			app_process_action (ACTION_CHOOSE);
	}
	return true;
}

static bool
app_process_mouse (termo_mouse_event_t type, int line, int column, int button,
	bool double_click)
{
	if (type != TERMO_MOUSE_PRESS)
		return true;

	if (button == 1)
		return app_process_left_mouse_click (line, column, double_click);
	else if (button == 4)
		return app_process_action (ACTION_SCROLL_UP);
	else if (button == 5)
		return app_process_action (ACTION_SCROLL_DOWN);
	return false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct binding
{
	const char *key;                    ///< Key definition
	enum action action;                 ///< Action to take
	termo_key_t decoded;                ///< Decoded key definition
}
g_default_bindings[] =
{
	{ "Escape",     ACTION_QUIT,               {}},
	{ "q",          ACTION_QUIT,               {}},
	{ "C-l",        ACTION_REDRAW,             {}},
	{ "M-Tab",      ACTION_LAST_TAB,           {}},
	{ "F1",         ACTION_HELP_TAB,           {}},

	{ "Home",       ACTION_GOTO_TOP,           {}},
	{ "End",        ACTION_GOTO_BOTTOM,        {}},
	{ "M-<",        ACTION_GOTO_TOP,           {}},
	{ "M->",        ACTION_GOTO_BOTTOM,        {}},
	{ "Up",         ACTION_GOTO_ITEM_PREVIOUS, {}},
	{ "Down",       ACTION_GOTO_ITEM_NEXT,     {}},
	{ "k",          ACTION_GOTO_ITEM_PREVIOUS, {}},
	{ "j",          ACTION_GOTO_ITEM_NEXT,     {}},
	{ "PageUp",     ACTION_GOTO_PAGE_PREVIOUS, {}},
	{ "PageDown",   ACTION_GOTO_PAGE_NEXT,     {}},
	{ "C-p",        ACTION_GOTO_ITEM_PREVIOUS, {}},
	{ "C-n",        ACTION_GOTO_ITEM_NEXT,     {}},
	{ "C-b",        ACTION_GOTO_PAGE_PREVIOUS, {}},
	{ "C-f",        ACTION_GOTO_PAGE_NEXT,     {}},

	{ "H",          ACTION_GOTO_VIEW_TOP,      {}},
	{ "M",          ACTION_GOTO_VIEW_CENTER,   {}},
	{ "L",          ACTION_GOTO_VIEW_BOTTOM,   {}},

	// Not sure how to set these up, they're pretty arbitrary so far
	{ "Enter",      ACTION_CHOOSE,             {}},
	{ "Delete",     ACTION_DELETE,             {}},
	{ "Backspace",  ACTION_UP,                 {}},
	{ "a",          ACTION_MPD_ADD,            {}},
	{ "r",          ACTION_MPD_REPLACE,        {}},

	{ "Left",       ACTION_MPD_PREVIOUS,       {}},
	{ "Right",      ACTION_MPD_NEXT,           {}},
	{ "M-Left",     ACTION_MPD_BACKWARD,       {}},
	{ "M-Right",    ACTION_MPD_FORWARD,        {}},
	{ "h",          ACTION_MPD_PREVIOUS,       {}},
	{ "l",          ACTION_MPD_NEXT,           {}},
	{ "Space",      ACTION_MPD_TOGGLE,         {}},
	{ "C-Space",    ACTION_MPD_STOP,           {}},
	{ "u",          ACTION_MPD_UPDATE_DB,      {}},
	{ "M-PageUp",   ACTION_MPD_VOLUME_UP,      {}},
	{ "M-PageDown", ACTION_MPD_VOLUME_DOWN,    {}},
};

static int
app_binding_cmp (const void *a, const void *b)
{
	return termo_keycmp (g.tk,
		&((struct binding *) a)->decoded, &((struct binding *) b)->decoded);
}

static void
app_init_bindings (void)
{
	for (size_t i = 0; i < N_ELEMENTS (g_default_bindings); i++)
	{
		struct binding *binding = &g_default_bindings[i];
		hard_assert (!*termo_strpkey_utf8 (g.tk,
			binding->key, &binding->decoded, TERMO_FORMAT_ALTISMETA));
	}
	qsort (g_default_bindings, N_ELEMENTS (g_default_bindings),
		sizeof *g_default_bindings, app_binding_cmp);
}

static bool
app_process_termo_event (termo_key_t *event)
{
	struct binding dummy = { NULL, 0, *event }, *binding =
		bsearch (&dummy, g_default_bindings, N_ELEMENTS (g_default_bindings),
			sizeof *g_default_bindings, app_binding_cmp);
	if (binding)
		return app_process_action (binding->action);

	// TODO: parametrize actions, put this among other bindings
	if (!(event->modifiers & ~TERMO_KEYMOD_ALT)
	 && event->code.codepoint >= '0'
	 && event->code.codepoint <= '9')
	{
		int n = event->code.codepoint - '0';
		if (app_goto_tab ((n == 0 ? 10 : n) - 1))
			return true;
	}
	return false;
}

// --- Current tab -------------------------------------------------------------

static struct tab g_current_tab;

static void
current_tab_on_item_draw (size_t item_index, struct row_buffer *buffer,
	int width)
{
	// TODO: configurable output, maybe dynamically sized columns
	int length_len = 1 /*separator */ + 2 /* h */ + 3 /* m */+ 3 /* s */;

	compact_map_t map = item_list_get (&g.playlist, item_index);
	const char *artist = compact_map_find (map, "artist");
	const char *title  = compact_map_find (map, "title");

	chtype attrs = (int) item_index == g.song ? A_BOLD : 0;
	if (artist && title)
		row_buffer_append_args (buffer,
			artist, attrs, " - ", attrs, title, attrs, NULL);
	else
		row_buffer_append (buffer, compact_map_find (map, "file"), attrs);

	row_buffer_align (buffer, width - length_len, attrs);

	char *s = NULL;
	unsigned long n;
	const char *time = compact_map_find (map, "time");
	if (!time || !xstrtoul (&n, time, 10) || !(s = app_time_string (n)))
		s = xstrdup ("?");

	char *right_aligned = xstrdup_printf ("%*s", length_len, s);
	row_buffer_append (buffer, right_aligned, attrs);
	free (right_aligned);
	free (s);
}

static bool
current_tab_on_action (enum action action)
{
	struct tab *self = g.active_tab;
	compact_map_t map = item_list_get (&g.playlist, self->item_selected);

	const char *id;
	if (!map || !(id = compact_map_find (map, "id")))
		return false;

	// TODO: add actions to move the current selection up or down with Shift,
	//   with multiple items we need to use all number indexes, but "moveid"
	switch (action)
	{
	case ACTION_CHOOSE:    return MPD_SIMPLE ("playid",   id);
	case ACTION_DELETE:    return MPD_SIMPLE ("deleteid", id);
	default:
		break;
	}
	return false;
}

static void
current_tab_update (void)
{
	g_current_tab.item_count = g.playlist.len;
	app_invalidate ();
}

static struct tab *
current_tab_init (void)
{
	struct tab *super = &g_current_tab;
	tab_init (super, "Current");
	super->on_action = current_tab_on_action;
	super->on_item_draw = current_tab_on_item_draw;
	return super;
}

// --- Library tab -------------------------------------------------------------

struct library_level
{
	LIST_HEADER (struct library_level)

	int item_top;                       ///< Stored state
	int item_selected;                  ///< Stored state
	char path[];                        ///< Path of the level
};

static struct
{
	struct tab super;                   ///< Parent class
	struct str path;                    ///< Current path
	struct strv items;                  ///< Current items (type, name, path)
	struct library_level *above;        ///< Upper levels
}
g_library_tab;

enum
{
	// This list is also ordered by ASCII and important for sorting

	LIBRARY_ROOT = '/',                 ///< Root entry
	LIBRARY_UP   = '^',                 ///< Upper directory
	LIBRARY_DIR  = 'd',                 ///< Directory
	LIBRARY_FILE = 'f'                  ///< File
};

struct library_tab_item
{
	int type;                           ///< Type of the item
	const char *name;                   ///< Visible name
	const char *path;                   ///< MPD path
};

static void
library_tab_add (int type, const char *name, const char *path)
{
	strv_append_owned (&g_library_tab.items,
		xstrdup_printf ("%c%s%c%s", type, name, 0, path));
}

static struct library_tab_item
library_tab_resolve (const char *raw)
{
	struct library_tab_item item;
	item.type = *raw++;
	item.name = raw;
	item.path = strchr (raw, '\0') + 1;
	return item;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
library_tab_on_item_draw (size_t item_index, struct row_buffer *buffer,
	int width)
{
	(void) width;
	hard_assert (item_index < g_library_tab.items.len);

	struct library_tab_item x =
		library_tab_resolve (g_library_tab.items.vector[item_index]);
	const char *prefix, *name;
	switch (x.type)
	{
	case LIBRARY_ROOT: prefix = "/"; name = "";     break;
	case LIBRARY_UP:   prefix = "/"; name = "..";   break;
	case LIBRARY_DIR:  prefix = "/"; name = x.name; break;
	case LIBRARY_FILE: prefix = " "; name = x.name; break;
	default:           hard_assert (!"invalid item type");
	}
	chtype attrs = x.type != LIBRARY_FILE ? APP_ATTR (DIRECTORY) : 0;
	row_buffer_append_args (buffer, prefix, attrs, name, attrs, NULL);
}

static char
library_tab_header_type (const char *key)
{
	if (!strcasecmp_ascii (key, "file"))      return LIBRARY_FILE;
	if (!strcasecmp_ascii (key, "directory")) return LIBRARY_DIR;
	return 0;
}

static void
library_tab_chunk (char type, const char *path, struct str_map *map)
{
	const char *artist = str_map_find (map, "artist");
	const char *title  = str_map_find (map, "title");
	char *name = (artist && title)
		? xstrdup_printf ("%s - %s", artist, title)
		: xstrdup (xbasename (path));
	library_tab_add (type, name, path);
	free (name);
}

static int
library_tab_compare (char **a, char **b)
{
	struct library_tab_item xa = library_tab_resolve (*a);
	struct library_tab_item xb = library_tab_resolve (*b);

	if (xa.type != xb.type)
		return xa.type - xb.type;

	return app_casecmp ((uint8_t *) xa.path, (uint8_t *) xb.path);
}

static char *
library_tab_parent (void)
{
	struct str *path = &g_library_tab.path;
	if (!path->len)
		return NULL;

	char *last_slash;
	if ((last_slash = strrchr (path->str, '/')))
		return xstrndup (path->str, last_slash - path->str);
	return xstrdup ("");
}

static bool
library_tab_is_above (const char *above, const char *path)
{
	size_t above_len = strlen (above);
	if (strncmp (above, path, above_len))
		return false;
	// The root is an empty string and is above anything other than itself
	return path[above_len] == '/' || (*path && !*above);
}

static void
library_tab_change_level (const char *new_path)
{
	struct str *path = &g_library_tab.path;
	if (!strcmp (path->str, new_path))
		return;

	struct library_level *above;
	if (library_tab_is_above (path->str, new_path))
	{
		above = xcalloc (1, sizeof *above + path->len + 1);
		above->item_top = g_library_tab.super.item_top;
		above->item_selected = g_library_tab.super.item_selected;
		memcpy (above->path, path->str, path->len);
		LIST_PREPEND (g_library_tab.above, above);

		// Select the ".." entry to reflect Norton Commander
		g_library_tab.super.item_top = 0;
		g_library_tab.super.item_selected = 1;
	}
	else while ((above = g_library_tab.above)
		&& !library_tab_is_above (above->path, new_path))
	{
		if (!strcmp (above->path, new_path))
		{
			g_library_tab.super.item_top = above->item_top;
			g_library_tab.super.item_selected = above->item_selected;
		}
		g_library_tab.above = above->next;
		free (above);
	}

	str_reset (path);
	str_append (path, new_path);

	free (g_library_tab.super.header);
	g_library_tab.super.header = NULL;

	if (path->len)
		g_library_tab.super.header = xstrdup_printf ("/%s", path->str);
}

static void
library_tab_on_data (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	char *new_path = user_data;
	if (!response->success)
	{
		print_error ("cannot read directory: %s", response->message_text);
		free (new_path);
		return;
	}

	strv_reset (&g_library_tab.items);
	library_tab_change_level (new_path);
	free (new_path);

	char *parent = library_tab_parent ();
	if (parent)
	{
		library_tab_add (LIBRARY_ROOT, "", "");
		library_tab_add (LIBRARY_UP, "", parent);
		free (parent);
	}

	struct str_map map;
	str_map_init (&map);
	map.key_xfrm = tolower_ascii_strxfrm;

	char *key, *value, type;
	for (size_t i = data->len; i--; )
		if (!(key = mpd_parse_kv (data->vector[i], &value)))
			continue;
		else if (!(type = library_tab_header_type (key)))
			str_map_set (&map, key, value);
		else
		{
			library_tab_chunk (type, value, &map);
			str_map_clear (&map);
		}
	str_map_free (&map);

	struct strv *items = &g_library_tab.items;
	qsort (items->vector, items->len, sizeof *items->vector,
		(int (*) (const void *, const void *)) library_tab_compare);
	g_library_tab.super.item_count = items->len;

	// Don't force the selection visible when there's no need to touch it
	if (g_library_tab.super.item_selected >= (int) items->len)
		app_move_selection (0);

	app_invalidate ();
}

static void
library_tab_reload (const char *new_path)
{
	char *path = new_path
		? xstrdup (new_path)
		: xstrdup (g_library_tab.path.str);

	struct mpd_client *c = &g.client;
	mpd_client_send_command (c, "lsinfo", *path ? path : "/", NULL);
	mpd_client_add_task (c, library_tab_on_data, path);
	mpd_client_idle (c, 0);
}

static bool
library_tab_on_action (enum action action)
{
	struct tab *self = g.active_tab;
	if (self->item_selected < 0 || !self->item_count)
		return false;

	struct mpd_client *c = &g.client;
	if (c->state != MPD_CONNECTED)
		return false;

	struct library_tab_item x =
		library_tab_resolve (g_library_tab.items.vector[self->item_selected]);
	switch (action)
	{
	case ACTION_CHOOSE:
		switch (x.type)
		{
		case LIBRARY_ROOT:
		case LIBRARY_UP:
		case LIBRARY_DIR:  library_tab_reload (x.path); break;
		case LIBRARY_FILE: MPD_SIMPLE ("add", x.path);  break;
		default:           hard_assert (!"invalid item type");
		}
		return true;
	case ACTION_UP:
	{
		char *parent = library_tab_parent ();
		if (parent)
		{
			library_tab_reload (parent);
			free (parent);
		}
		return parent != NULL;
	}
	case ACTION_MPD_REPLACE:
		if (x.type != LIBRARY_DIR && x.type != LIBRARY_FILE)
			break;

		// Clears the playlist (which stops playback), add what user wanted
		// to replace it with, and eventually restore playback;
		// I can't think of a reliable alternative that omits the "play"
		mpd_client_list_begin (c);
		mpd_client_send_command (c, "clear", NULL);
		mpd_client_send_command (c, "add", x.path, NULL);
		if (g.state == PLAYER_PLAYING)
			mpd_client_send_command (c, "play", NULL);
		mpd_client_list_end (c);
		mpd_client_add_task (c, mpd_on_simple_response, NULL);
		mpd_client_idle (c, 0);
		return true;
	case ACTION_MPD_ADD:
		if (x.type != LIBRARY_DIR && x.type != LIBRARY_FILE)
			break;

		return MPD_SIMPLE ("add", x.path);
	default:
		break;
	}
	return false;
}

static struct tab *
library_tab_init (void)
{
	str_init (&g_library_tab.path);
	strv_init (&g_library_tab.items);

	struct tab *super = &g_library_tab.super;
	tab_init (super, "Library");
	super->on_action = library_tab_on_action;
	super->on_item_draw = library_tab_on_item_draw;
	return super;
}

// --- Streams -----------------------------------------------------------------

// MPD can only parse m3u8 playlists, and only when it feels like doing so

struct stream_tab_task
{
	struct poller_curl_task curl;       ///< Superclass
	struct str data;                    ///< Downloaded data
	bool polling;                       ///< Still downloading
	bool replace;                       ///< Should playlist be replaced?
};

static bool
is_content_type (const char *content_type,
	const char *expected_type, const char *expected_subtype)
{
	char *type = NULL, *subtype = NULL;
	bool result = http_parse_media_type (content_type, &type, &subtype, NULL)
		&& !strcasecmp_ascii (type, expected_type)
		&& !strcasecmp_ascii (subtype, expected_subtype);
	free (type);
	free (subtype);
	return result;
}

static void
streams_tab_parse_playlist (const char *playlist, const char *content_type,
	struct strv *out)
{
	// We accept a lot of very broken stuff because this is the real world
	struct strv lines;
	strv_init (&lines);
	cstr_split (playlist, "\r\n", true, &lines);

	// Since this excludes '"', it should even work for XMLs (w/o entities)
	const char *extract_re =
		"(https?://([][a-z0-9._~:/?#@!$&'()*+,;=-]|%[a-f0-9]{2})+)";
	if ((lines.len && !strcasecmp_ascii (lines.vector[0], "[playlist]"))
	 || (content_type && is_content_type (content_type, "audio", "x-scpls")))
		extract_re = "^File[^=]*=(.*)";
	else if ((lines.len && !strcasecmp_ascii (lines.vector[0], "#EXTM3U"))
	 || (content_type && is_content_type (content_type, "audio", "x-mpegurl")))
		extract_re = "^([^#].*)";

	regex_t *re = regex_compile (extract_re, REG_EXTENDED, NULL);
	hard_assert (re != NULL);

	regmatch_t groups[2];
	for (size_t i = 0; i < lines.len; i++)
		if (regexec (re, lines.vector[i], 2, groups, 0) != REG_NOMATCH)
		{
			char *target = xstrndup (lines.vector[i] + groups[1].rm_so,
				groups[1].rm_eo - groups[1].rm_so);
			if (utf8_validate (target, strlen (target)))
				strv_append_owned (out, target);
			else
			{
				strv_append_owned (out, latin1_to_utf8 (target));
				free (target);
			}
		}
	regex_free (re);
	strv_free (&lines);
}

static bool
streams_tab_extract_links (struct str *data, const char *content_type,
	struct strv *out)
{
	// Since playlists are also "audio/*", this seems like a sane thing to do
	for (size_t i = 0; i < data->len; i++)
	{
		uint8_t c = data->str[i];
		if ((c < 32) & (c != '\t') & (c != '\r') & (c != '\n'))
			return false;
	}

	streams_tab_parse_playlist (data->str, content_type, out);
	return true;
}

static void
streams_tab_on_downloaded (CURLMsg *msg, struct poller_curl_task *task)
{
	struct stream_tab_task *self =
		CONTAINER_OF (task, struct stream_tab_task, curl);
	self->polling = false;

	if (msg->data.result
	 && msg->data.result != CURLE_WRITE_ERROR)
	{
		cstr_uncapitalize (self->curl.curl_error);
		print_error ("%s", self->curl.curl_error);
		return;
	}

	struct mpd_client *c = &g.client;
	if (c->state != MPD_CONNECTED)
		return;

	CURL *easy = msg->easy_handle;
	CURLcode res;

	long code;
	char *type, *uri;
	if ((res = curl_easy_getinfo (easy, CURLINFO_RESPONSE_CODE, &code))
	 || (res = curl_easy_getinfo (easy, CURLINFO_CONTENT_TYPE, &type))
	 || (res = curl_easy_getinfo (easy, CURLINFO_EFFECTIVE_URL, &uri)))
	{
		print_error ("%s: %s",
			"cURL info retrieval failed", curl_easy_strerror (res));
		return;
	}
	// cURL is not willing to parse the ICY header, the code is zero then
	if (code && code != 200)
	{
		print_error ("%s: %ld", "unexpected HTTP response", code);
		return;
	}

	mpd_client_list_begin (c);
	if (self->replace)
		mpd_client_send_command (c, "clear", NULL);

	struct strv links;
	strv_init (&links);

	if (!streams_tab_extract_links (&self->data, type, &links))
		strv_append (&links, uri);
	for (size_t i = 0; i < links.len; i++)
		mpd_client_send_command (c, "add", links.vector[i], NULL);
	if (self->replace && g.state == PLAYER_PLAYING)
		mpd_client_send_command (c, "play", NULL);

	strv_free (&links);
	mpd_client_list_end (c);
	mpd_client_add_task (c, mpd_on_simple_response, NULL);
	mpd_client_idle (c, 0);
}

static size_t
write_callback (char *ptr, size_t size, size_t nmemb, void *user_data)
{
	struct str *buf = user_data;
	str_append_data (buf, ptr, size * nmemb);

	// Invoke CURLE_WRITE_ERROR when we've received enough data for a playlist
	if (buf->len >= (1 << 16))
		return 0;

	return size * nmemb;
}

static bool
streams_tab_process (const char *uri, bool replace, struct error **e)
{
	struct poller poller;
	poller_init (&poller);

	struct poller_curl pc;
	hard_assert (poller_curl_init (&pc, &poller, NULL));
	struct stream_tab_task task;
	hard_assert (poller_curl_spawn (&task.curl, NULL));

	CURL *easy = task.curl.easy;
	str_init (&task.data);
	task.replace = replace;
	bool result = false;

	CURLcode res;
	if ((res = curl_easy_setopt (easy, CURLOPT_FOLLOWLOCATION, 1L))
	 || (res = curl_easy_setopt (easy, CURLOPT_NOPROGRESS,     1L))
	// TODO: make the timeout a bit larger once we're asynchronous
	 || (res = curl_easy_setopt (easy, CURLOPT_TIMEOUT,        5L))
	// Not checking anything, we just want some data, any data
	 || (res = curl_easy_setopt (easy, CURLOPT_SSL_VERIFYPEER, 0L))
	 || (res = curl_easy_setopt (easy, CURLOPT_SSL_VERIFYHOST, 0L))
	 || (res = curl_easy_setopt (easy, CURLOPT_URL,            uri))

	 || (res = curl_easy_setopt (easy, CURLOPT_VERBOSE, (long) g_debug_mode))
	 || (res = curl_easy_setopt (easy, CURLOPT_DEBUGFUNCTION, print_curl_debug))
	 || (res = curl_easy_setopt (easy, CURLOPT_WRITEDATA, &task.data))
	 || (res = curl_easy_setopt (easy, CURLOPT_WRITEFUNCTION, write_callback)))
	{
		error_set (e, "%s: %s", "cURL setup failed", curl_easy_strerror (res));
		goto error;
	}

	task.curl.on_done = streams_tab_on_downloaded;
	hard_assert (poller_curl_add (&pc, task.curl.easy, NULL));

	// TODO: don't run a subloop, run the task fully asynchronously
	task.polling = true;
	while (task.polling)
		poller_run (&poller);

	hard_assert (poller_curl_remove (&pc, task.curl.easy, NULL));
	result = true;

error:
	curl_easy_cleanup (task.curl.easy);
	str_free (&task.data);
	poller_curl_free (&pc);

	poller_free (&poller);
	return result;
}

static bool
streams_tab_on_action (enum action action)
{
	struct tab *self = g.active_tab;
	if (self->item_selected < 0 || !self->item_count)
		return false;

	// For simplicity the URL is the string following the stream name
	const char *uri = 1 + strchr (g.streams.vector[self->item_selected], 0);

	struct error *e = NULL;
	switch (action)
	{
	case ACTION_MPD_REPLACE:
		streams_tab_process (uri, true,  &e);
		break;
	case ACTION_CHOOSE:
	case ACTION_MPD_ADD:
		streams_tab_process (uri, false, &e);
		break;
	default:
		return false;
	}
	if (e)
	{
		print_error ("%s", e->message);
		error_free (e);
	}
	return true;
}

static void
streams_tab_on_item_draw (size_t item_index, struct row_buffer *buffer,
	int width)
{
	(void) width;
	row_buffer_append (buffer, g.streams.vector[item_index], 0);
}

static struct tab *
streams_tab_init (void)
{
	static struct tab super;
	tab_init (&super, "Streams");
	super.on_action = streams_tab_on_action;
	super.on_item_draw = streams_tab_on_item_draw;
	super.item_count = g.streams.len;
	return &super;
}

// --- Info tab ----------------------------------------------------------------

static struct
{
	struct tab super;                   ///< Parent class
	struct strv keys;                   ///< Data keys
	struct strv values;                 ///< Data values
}
g_info_tab;

static void
info_tab_on_item_draw (size_t item_index, struct row_buffer *buffer, int width)
{
	(void) width;

	// It looks like we could do with a generic list structure that just
	// stores formatted row_buffers.  Let's see for other tabs:
	//  - Current -- unusable, has dynamic column alignment
	//  - Library -- could work for the "icons"
	//  - Streams -- useless
	//  - Debug   -- it'd take up considerably more space
	// However so far we're only showing show key-value pairs.

	row_buffer_append_args (buffer,
		g_info_tab.keys.vector[item_index], A_BOLD, ":", A_BOLD, NULL);
	row_buffer_space (buffer, 8 - buffer->total_width, 0);
	row_buffer_append (buffer, g_info_tab.values.vector[item_index], 0);
}

static void
info_tab_add (compact_map_t data, const char *field)
{
	const char *value = compact_map_find (data, field);
	if (!value) value = "";

	strv_append (&g_info_tab.keys, field);
	strv_append (&g_info_tab.values, value);
	g_info_tab.super.item_count++;
}

static void
info_tab_update (void)
{
	strv_reset (&g_info_tab.keys);
	strv_reset (&g_info_tab.values);
	g_info_tab.super.item_count = 0;

	compact_map_t map;
	if ((map = item_list_get (&g.playlist, g.song)))
	{
		info_tab_add (map, "Title");
		info_tab_add (map, "Artist");
		info_tab_add (map, "Album");
		info_tab_add (map, "Track");
		info_tab_add (map, "Genre");
		// Yes, it is "file", but this is also for display
		info_tab_add (map, "File");
	}
}

static struct tab *
info_tab_init (void)
{
	strv_init (&g_info_tab.keys);
	strv_init (&g_info_tab.values);

	struct tab *super = &g_info_tab.super;
	tab_init (super, "Info");
	super->on_item_draw = info_tab_on_item_draw;
	return super;
}

// --- Help tab ----------------------------------------------------------------

static void
help_tab_on_item_draw (size_t item_index, struct row_buffer *buffer, int width)
{
	(void) width;

	// TODO: group them the other way around for clarity
	hard_assert (item_index < N_ELEMENTS (g_default_bindings));
	struct binding *binding = &g_default_bindings[item_index];
	char *text = xstrdup_printf ("%-12s %s",
		binding->key, g_actions[binding->action].description);
	row_buffer_append (buffer, text, 0);
	free (text);
}

static struct tab *
help_tab_init (void)
{
	static struct tab super;
	tab_init (&super, "Help");
	super.on_item_draw = help_tab_on_item_draw;
	super.item_count = N_ELEMENTS (g_default_bindings);
	return &super;
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
	ARRAY (struct debug_item, items)    ///< Items
	bool active;                        ///< The tab is present
}
g_debug_tab;

static void
debug_tab_on_item_draw (size_t item_index, struct row_buffer *buffer, int width)
{
	hard_assert (item_index < g_debug_tab.items_len);
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
	row_buffer_align (buffer, width, item->attrs);
}

static void
debug_tab_push (char *message, chtype attrs)
{
	ARRAY_RESERVE (g_debug_tab.items, 1);
	struct debug_item *item = &g_debug_tab.items[g_debug_tab.items_len++];
	g_debug_tab.super.item_count = g_debug_tab.items_len;
	item->text = message;
	item->attrs = attrs;
	item->timestamp = clock_msec (CLOCK_REALTIME);

	app_invalidate ();
}

static struct tab *
debug_tab_init (void)
{
	ARRAY_INIT (g_debug_tab.items);
	g_debug_tab.active = true;

	struct tab *super = &g_debug_tab.super;
	tab_init (super, "Debug");
	super->on_item_draw = debug_tab_on_item_draw;
	return super;
}

// --- MPD interface -----------------------------------------------------------

static void
mpd_read_time (const char *value, int *sec, int *optional_msec)
{
	if (!value)
		return;

	char *end, *period = strchr (value, '.');
	if (optional_msec && period)
	{
		unsigned long n = strtoul (period + 1, &end, 10);
		if (*end)
			return;
		*optional_msec = MIN (INT_MAX, n);
	}
	unsigned long n = strtoul (value, &end, 10);
	if (end == period || !*end)
		*sec = MIN (INT_MAX, n);
}

static void
mpd_update_playlist_time (void)
{
	g.playlist_time = 0;

	// It would also be possible to retrieve this from "stats" -> "playtime"
	unsigned long n;
	for (size_t i = 0; i < g.playlist.len; i++)
	{
		compact_map_t map = item_list_get (&g.playlist, i);
		const char *time = compact_map_find (map, "time");
		if (time && xstrtoul (&n, time, 10))
			g.playlist_time += n;
	}
}

static void
mpd_update_playback_state (void)
{
	struct str_map *map = &g.playback_info;
	g.song_elapsed = g.song_duration = g.volume = g.song = -1;
	uint32_t last_playlist_version = g.playlist_version;
	g.playlist_version = 0;

	const char *state;
	g.state = PLAYER_STOPPED;
	if ((state = str_map_find (map, "state")))
	{
		if (!strcmp (state, "play"))   g.state = PLAYER_PLAYING;
		if (!strcmp (state, "pause"))  g.state = PLAYER_PAUSED;
	}

	// Values in "time" are always rounded.  "elapsed", introduced in MPD 0.16,
	// is in millisecond precision and "duration" as well, starting with 0.20.
	// Prefer the more precise values but use what we have.
	const char *time     = str_map_find (map, "time");
	const char *elapsed  = str_map_find (map, "elapsed");
	const char *duration = str_map_find (map, "duration");

	struct strv fields;
	strv_init (&fields);
	if (time)
	{
		cstr_split (time, ":", false, &fields);
		if (fields.len >= 1 && !elapsed)   elapsed  = fields.vector[0];
		if (fields.len >= 2 && !duration)  duration = fields.vector[1];
	}

	int msec_past_second = 0;
	mpd_read_time (elapsed,  &g.song_elapsed,  &msec_past_second);
	mpd_read_time (duration, &g.song_duration, NULL);
	strv_free (&fields);

	// We could also just poll the server each half a second but let's not
	poller_timer_reset (&g.elapsed_event);
	if (g.state == PLAYER_PLAYING)
	{
		poller_timer_set (&g.elapsed_event, 1000 - msec_past_second);
		g.elapsed_since = clock_msec (CLOCK_BEST) - msec_past_second;
	}

	// The server sends -1 when nothing is being played right now
	unsigned long n;
	if (xstrtoul_map (map, "volume",   &n))  g.volume           = n;

	if (xstrtoul_map (map, "playlist", &n))  g.playlist_version = n;
	if (xstrtoul_map (map, "song",     &n))  g.song             = n;

	if (g.playlist_version != last_playlist_version)
		mpd_update_playlist_time ();

	app_invalidate ();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_process_info (const struct strv *data)
{
	struct str_map *map = &g.playback_info;

	// First there's the status, followed by playlist items chunked by "file"
	unsigned long n; char *key, *value;
	for (size_t i = 0; i < data->len - 1 && data->vector[i]; i++)
	{
		if (!(key = mpd_parse_kv (data->vector[i], &value)))
			continue;
		if (!strcasecmp_ascii (key, "playlistlength")
			&& xstrtoul (&n, value, 10))
			item_list_resize (&g.playlist, n);
		str_map_set (map, key, xstrdup (value));
	}

	// It's much better to process the playlist from the back
	struct str_map item;
	str_map_init (&item);
	item.key_xfrm = tolower_ascii_strxfrm;
	for (size_t i = data->len - 1; i-- && data->vector[i]; )
	{
		if (!(key = mpd_parse_kv (data->vector[i], &value)))
			continue;
		str_map_set (&item, key, value);
		if (!strcasecmp_ascii (key, "file"))
		{
			if (xstrtoul_map (&item, "pos", &n))
				item_list_set (&g.playlist, n, &item);
			str_map_clear (&item);
		}
	}
	str_map_free (&item);
}

static void
mpd_on_info_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) user_data;

	// TODO: preset an error player state?
	str_map_clear (&g.playback_info);
	if (!response->success)
		print_error ("%s: %s",
			"MPD status retrieval failed", response->message_text);
	else if (!data->len)
		print_debug ("empty MPD status response");
	else
		mpd_process_info (data);

	mpd_update_playback_state ();
	current_tab_update ();
	info_tab_update ();
}

static void
mpd_on_tick (void *user_data)
{
	(void) user_data;
	int64_t diff_msec = clock_msec (CLOCK_BEST) - g.elapsed_since;
	int elapsed_sec = diff_msec / 1000;
	int elapsed_msec = diff_msec % 1000;

	g.song_elapsed += elapsed_sec;
	g.elapsed_since += elapsed_sec * 1000;
	poller_timer_set (&g.elapsed_event, 1000 - elapsed_msec);

	app_invalidate ();
}

static void
mpd_request_info (void)
{
	struct mpd_client *c = &g.client;

	mpd_client_list_ok_begin (c);
	mpd_client_send_command (c, "status", NULL);
	char *last_version = xstrdup_printf ("%" PRIu32, g.playlist_version);
	mpd_client_send_command (c, "plchanges", last_version, NULL);
	free (last_version);
	mpd_client_list_end (c);
	mpd_client_add_task (c, mpd_on_info_response, NULL);
	mpd_client_idle (c, 0);
}

static void
mpd_on_events (unsigned subsystems, void *user_data)
{
	(void) user_data;
	struct mpd_client *c = &g.client;

	if (subsystems & MPD_SUBSYSTEM_DATABASE)
		library_tab_reload (NULL);

	if (subsystems & (MPD_SUBSYSTEM_PLAYER | MPD_SUBSYSTEM_OPTIONS
		| MPD_SUBSYSTEM_PLAYLIST | MPD_SUBSYSTEM_MIXER | MPD_SUBSYSTEM_UPDATE))
		mpd_request_info ();
	else
		mpd_client_idle (c, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_queue_reconnect (void)
{
	poller_timer_set (&g.connect_event, 5 * 1000);
}

static void
mpd_on_password_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) data;
	(void) user_data;
	struct mpd_client *c = &g.client;

	if (response->success)
	{
		mpd_request_info ();
		library_tab_reload (NULL);
	}
	else
	{
		print_error ("%s: %s",
			"MPD authentication failed", response->message_text);
		mpd_client_send_command (c, "close", NULL);
	}
}

static void
mpd_on_connected (void *user_data)
{
	(void) user_data;
	struct mpd_client *c = &g.client;

	const char *password =
		get_config_string (g.config.root, "settings.password");
	if (password)
	{
		mpd_client_send_command (c, "password", password, NULL);
		mpd_client_add_task (c, mpd_on_password_response, NULL);
	}
	else
	{
		mpd_request_info ();
		library_tab_reload (NULL);
	}
}

static void
mpd_on_failure (void *user_data)
{
	(void) user_data;
	// This is also triggered both by a failed connect and a clean disconnect
	print_debug ("connection to MPD failed");
	mpd_queue_reconnect ();

	str_map_clear (&g.playback_info);
	item_list_resize (&g.playlist, 0);

	mpd_update_playback_state ();
	current_tab_update ();
	info_tab_update ();
}

static void
mpd_on_io_hook (void *user_data, bool outgoing, const char *line)
{
	(void) user_data;
	if (outgoing)
		debug_tab_push (xstrdup_printf ("<< %s", line), APP_ATTR (OUTGOING));
	else
		debug_tab_push (xstrdup_printf (">> %s", line), APP_ATTR (INCOMING));
}

static void
app_on_reconnect (void *user_data)
{
	(void) user_data;

	struct mpd_client *c = &g.client;
	c->on_failure   = mpd_on_failure;
	c->on_connected = mpd_on_connected;
	c->on_event     = mpd_on_events;

	if (g_debug_mode)
		c->on_io_hook = mpd_on_io_hook;

	// We accept hostname/IPv4/IPv6 in pseudo-URL format, as well as sockets
	char *address = xstrdup (get_config_string (g.config.root,
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
app_on_tty_event (termo_key_t *event, int64_t event_ts)
{
	// Simple double click detection via release--press delay, only a bit
	// complicated by the fact that we don't know what's being released
	static termo_key_t last_event;
	static int64_t last_event_ts;
	static int last_button;

	int y, x, button, y_last, x_last;
	termo_mouse_event_t type, type_last;
	if (termo_interpret_mouse (g.tk, event, &type, &button, &y, &x))
	{
		bool double_click = termo_interpret_mouse
			(g.tk, &last_event, &type_last, NULL, &y_last, &x_last)
			&& event_ts - last_event_ts < 500
			&& type_last == TERMO_MOUSE_RELEASE && type == TERMO_MOUSE_PRESS
			&& y_last == y && x_last == x && last_button == button;
		if (!app_process_mouse (type, y, x, button, double_click))
			beep ();

		// Prevent interpreting triple clicks as two double clicks
		if (double_click)
			last_button = 0;
		else if (type == TERMO_MOUSE_PRESS)
			last_button = button;
	}
	else if (!app_process_termo_event (event))
		beep ();

	last_event = *event;
	last_event_ts = event_ts;
}

static void
app_on_tty_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;
	if (fd->revents & ~(POLLIN | POLLHUP | POLLERR))
		print_debug ("fd %d: unexpected revents: %d", fd->fd, fd->revents);

	poller_timer_reset (&g.tk_timer);
	termo_advisereadable (g.tk);

	termo_key_t event;
	int64_t event_ts = clock_msec (CLOCK_BEST);
	termo_result_t res;
	while ((res = termo_getkey (g.tk, &event)) == TERMO_RES_KEY)
		app_on_tty_event (&event, event_ts);

	if (res == TERMO_RES_AGAIN)
		poller_timer_set (&g.tk_timer, termo_get_waittime (g.tk));
	else if (res == TERMO_RES_ERROR || res == TERMO_RES_EOF)
		app_quit ();
}

static void
app_on_key_timer (void *user_data)
{
	(void) user_data;

	termo_key_t event;
	if (termo_getkey_force (g.tk, &event) == TERMO_RES_KEY)
		if (!app_process_termo_event (&event))
			app_quit ();
}

static void
app_on_signal_pipe_readable (const struct pollfd *fd, void *user_data)
{
	(void) user_data;

	char id = 0;
	(void) read (fd->fd, &id, 1);

	if (g_termination_requested && !g.quitting)
		app_quit ();

	if (g_winch_received)
	{
		g_winch_received = false;
		update_curses_terminal_size ();
		app_invalidate ();
	}
}

static void
app_on_message_timer (void *user_data)
{
	(void) user_data;

	free (g.message);
	g.message = NULL;
	app_invalidate ();
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
	// the message to the user
	if (!isatty (STDERR_FILENO))
		fprintf (stderr, "%s\n", message.str);
	else if (g_debug_tab.active)
		debug_tab_push (str_steal (&message),
			user_data == NULL ? 0 : g.attrs[(intptr_t) user_data].attrs);
	else
	{
		free (g.message);
		g.message = xstrdup (message.str);
		app_invalidate ();
		poller_timer_set (&g.message_timer, 5000);
	}
	str_free (&message);

	in_processing = false;
}

static void
app_init_poller_events (void)
{
	poller_fd_init (&g.signal_event, &g.poller, g_signal_pipe[0]);
	g.signal_event.dispatcher = app_on_signal_pipe_readable;
	poller_fd_set (&g.signal_event, POLLIN);

	poller_fd_init (&g.tty_event, &g.poller, STDIN_FILENO);
	g.tty_event.dispatcher = app_on_tty_readable;
	poller_fd_set (&g.tty_event, POLLIN);

	poller_timer_init (&g.message_timer, &g.poller);
	g.message_timer.dispatcher = app_on_message_timer;

	poller_timer_init (&g.tk_timer, &g.poller);
	g.tk_timer.dispatcher = app_on_key_timer;

	poller_timer_init (&g.connect_event, &g.poller);
	g.connect_event.dispatcher = app_on_reconnect;
	poller_timer_set (&g.connect_event, 0);

	poller_timer_init (&g.elapsed_event, &g.poller);
	g.elapsed_event.dispatcher = mpd_on_tick;

	poller_idle_init (&g.refresh_event, &g.poller);
	g.refresh_event.dispatcher = app_on_refresh;
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
	signals_setup_handlers ();
	app_init_poller_events ();
	app_init_bindings ();

	if (g_debug_mode)
		app_prepend_tab (debug_tab_init ());

	// Redirect all messages from liberty to a special tab so they're not lost
	g_log_message_real = app_log_handler;

	app_prepend_tab (info_tab_init ());
	if (g.streams.len)
		app_prepend_tab (streams_tab_init ());
	app_prepend_tab (library_tab_init ());
	app_prepend_tab (current_tab_init ());
	app_switch_tab ((g.help_tab = help_tab_init ()));

	g.polling = true;
	while (g.polling)
		poller_run (&g.poller);

	endwin ();
	g_log_message_real = log_message_stdio;
	app_free_context ();
	return 0;
}
