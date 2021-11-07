/*
 * nncmpp -- the MPD client you never knew you needed
 *
 * Copyright (c) 2016 - 2021, Přemysl Eric Janouch <p@janouch.name>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
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
#define ATTRIBUTE_TABLE(XX)                              \
	XX( NORMAL,      normal,       -1, -1, 0           ) \
	XX( HIGHLIGHT,   highlight,    -1, -1, A_BOLD      ) \
	/* Gauge                                          */ \
	XX( ELAPSED,     elapsed,      -1, -1, A_REVERSE   ) \
	XX( REMAINS,     remains,      -1, -1, A_UNDERLINE ) \
	/* Tab bar                                        */ \
	XX( TAB_BAR,     tab_bar,      -1, -1, A_REVERSE   ) \
	XX( TAB_ACTIVE,  tab_active,   -1, -1, A_UNDERLINE ) \
	/* Listview                                       */ \
	XX( HEADER,      header,       -1, -1, A_UNDERLINE ) \
	XX( EVEN,        even,         -1, -1, 0           ) \
	XX( ODD,         odd,          -1, -1, 0           ) \
	XX( DIRECTORY,   directory,    -1, -1, 0           ) \
	XX( SELECTION,   selection,    -1, -1, A_REVERSE   ) \
	/* Cyan is good with both black and white.
	 * Can't use A_REVERSE because bold'd be bright.
	 * Unfortunately ran out of B&W attributes.       */ \
	XX( MULTISELECT, multiselect,  -1,  6, 0           ) \
	/* This ought to be indicative enough.            */ \
	XX( DEFOCUSED,   defocused,    -1, -1, A_UNDERLINE ) \
	XX( SCROLLBAR,   scrollbar,    -1, -1, 0           ) \
	/* These are for debugging only                   */ \
	XX( WARNING,     warning,       3, -1, 0           ) \
	XX( ERROR,       error,         1, -1, 0           ) \
	XX( INCOMING,    incoming,      2, -1, 0           ) \
	XX( OUTGOING,    outgoing,      4, -1, 0           )

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

#define HAVE_LIBERTY
#include "line-editor.c"

#include <math.h>
#include <locale.h>
#include <termios.h>
#include <sys/ioctl.h>

// ncurses is notoriously retarded for input handling, we need something
// different if only to receive mouse events reliably.
//
// 2021 update: ncurses is mostly reliable now, though rxvt-unicode only
// supports the 1006 mode that ncurses also supports mode starting with 9.25.

#include "termo.h"

// We need cURL to extract links from Internet stream playlists.  It'd be way
// too much code to do this all by ourselves, and there's nothing better around.

#include <curl/curl.h>

// The spectrum analyser requires a DFT transform.  The FFTW library is fairly
// efficient, and doesn't have a requirement on the number of bins.

#ifdef WITH_FFTW
#include <fftw3.h>
#endif  // WITH_FFTW

#define APP_TITLE  PROGRAM_NAME         ///< Left top corner

// --- Utilities ---------------------------------------------------------------

// The standard endwin/refresh sequence makes the terminal flicker
static void
update_curses_terminal_size (void)
{
#if defined HAVE_RESIZETERM && defined TIOCGWINSZ
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
	struct str converted = str_make ();
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
		copy[i] = !iscntrl_ascii (c) || c == '\n' ? c : '.';
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

static void
mpd_read_time (const char *value, int *sec, int *optional_msec)
{
	if (!value)
		return;

	char *end = NULL;
	long n = strtol (value, &end, 10);
	if (n < 0 || (*end && *end != '.'))
		return;

	int msec = 0;
	if (*end == '.')
	{
		// In practice, MPD always uses three decimal digits
		size_t digits = strspn (++end, "0123456789");
		if (end[digits])
			return;

		if (digits--) msec += (*end++ - '0') * 100;
		if (digits--) msec += (*end++ - '0') * 10;
		if (digits--) msec +=  *end++ - '0';
	}

	*sec = MIN (INT_MAX, n);
	if (optional_msec)
		*optional_msec = msec;
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

		fd->fd = poller_fd_make (self->poller, s);
		fd->fd.dispatcher = poller_curl_on_socket;
		fd->fd.user_data = self;
		curl_multi_assign (self->multi, s, fd);
	}
	if (what == CURL_POLL_REMOVE)
	{
		// Some annoying cURL bug.  Never trust libraries.
		fd->fd.closed = fcntl(fd->fd.fd, F_GETFL) < 0 && errno == EBADF;

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

	self->timer = poller_timer_make ((self->poller = poller));
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
	struct str s = str_make ();
	struct str_map_iter iter = str_map_iter_make (map);

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

static struct item_list
item_list_make (void)
{
	struct item_list self = {};
	self.items = xcalloc (sizeof *self.items, (self.alloc = 16));
	return self;
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

// --- Spectrum analyzer -------------------------------------------------------

// See http://www.zytrax.com/tech/audio/equalization.html
// for a good write-up about this problem domain

#ifdef WITH_FFTW

struct spectrum
{
	int sampling_rate;                  ///< Number of samples per seconds
	int channels;                       ///< Number of sampled channels
	int bits;                           ///< Number of bits per sample
	int bars;                           ///< Number of output vertical bars

	int bins;                           ///< Number of DFT bins
	int useful_bins;                    ///< Bins up to the Nyquist frequency
	int samples;                        ///< Number of windows to average
	float accumulator_scale;            ///< Scaling factor for accum. values
	int *top_bins;                      ///< Top DFT bin index for each bar
	char *spectrum;                     ///< String buffer for the "render"

	void *buffer;                       ///< Input buffer
	size_t buffer_len;                  ///< Input buffer fill level
	size_t buffer_size;                 ///< Input buffer size

	/// Decode the respective part of the buffer into the second half of data
	void (*decode) (struct spectrum *, int sample);

	float *data;                        ///< Normalized audio data
	float *window;                      ///< Sampled window function
	float *windowed;                    ///< data * window
	fftwf_complex *out;                 ///< DFT output
	fftwf_plan p;                       ///< DFT plan/FFTW configuration
	float *accumulator;                 ///< Accumulated powers of samples
};

// - - Windows - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Out: float[n] of 0..1
static void
window_hann (float *coefficients, size_t n)
{
	for (size_t i = 0; i < n; i++)
	{
		float sine = sin (M_PI * i / n);
		coefficients[i] = sine * sine;
	}
}

// In: float[n] of -1..1, float[n] of 0..1; out: float[n] of -1..1
static void
window_apply (const float *in, const float *coefficients, float *out, size_t n)
{
	for (size_t i = 0; i < n; i++)
		out[i] = in[i] * coefficients[i];
}

// In: float[n] of 0..1; out: float 0..n, describing the coherent gain
static float
window_coherent_gain (const float *in, size_t n)
{
	float sum = 0;
	for (size_t i = 0; i < n; i++)
		sum += in[i];
	return sum;
}

// - - Decoding  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
spectrum_decode_8 (struct spectrum *s, int sample)
{
	size_t n = s->useful_bins;
	float *data = s->data + n;
	for (int8_t *p = (int8_t *) s->buffer + sample * n * s->channels;
		n--; p += s->channels)
	{
		int32_t acc = 0;
		for (int ch = 0; ch < s->channels; ch++)
			acc += p[ch];
		*data++ = (float) acc / s->channels / -INT8_MIN;
	}
}

static void
spectrum_decode_16 (struct spectrum *s, int sample)
{
	size_t n = s->useful_bins;
	float *data = s->data + n;
	for (int16_t *p = (int16_t *) s->buffer + sample * n * s->channels;
		n--; p += s->channels)
	{
		int32_t acc = 0;
		for (int ch = 0; ch < s->channels; ch++)
			acc += p[ch];
		*data++ = (float) acc / s->channels / -INT16_MIN;
	}
}

static void
spectrum_decode_16_2 (struct spectrum *s, int sample)
{
	size_t n = s->useful_bins;
	float *data = s->data + n;
	for (int16_t *p = (int16_t *) s->buffer + sample * n * 2; n--; p += 2)
		*data++ = ((int32_t) p[0] + p[1]) / 2. / -INT16_MIN;
}

// - - Spectrum analysis - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static const char *spectrum_bars[] =
	{ " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };

/// Assuming the input buffer is full, updates the rendered spectrum
static void
spectrum_sample (struct spectrum *s)
{
	memset (s->accumulator, 0, sizeof *s->accumulator * s->useful_bins);

	// Credit for the algorithm goes to Audacity's /src/SpectrumAnalyst.cpp,
	// apparently Welch's method
	for (int sample = 0; sample < s->samples; sample++)
	{
		// We use 50% overlap and start with data from the last run (if any)
		memmove (s->data, s->data + s->useful_bins,
			sizeof *s->data * s->useful_bins);
		s->decode (s, sample);

		window_apply (s->data, s->window, s->windowed, s->bins);
		fftwf_execute (s->p);

		for (int bin = 0; bin < s->useful_bins; bin++)
		{
			// out[0][0] is the DC component, not useful to us
			float re = s->out[bin + 1][0];
			float im = s->out[bin + 1][1];
			s->accumulator[bin] += re * re + im * im;
		}
	}

	int last_bin = 0;
	char *p = s->spectrum;
	for (int bar = 0; bar < s->bars; bar++)
	{
		int top_bin = s->top_bins[bar];

		// Think of this as accumulating energies within bands,
		// so that it matches our non-linear hearing--there's no averaging.
		// For more precision, we could employ an "equal loudness contour".
		float acc = 0;
		for (int bin = last_bin; bin < top_bin; bin++)
			acc += s->accumulator[bin];

		last_bin = top_bin;
		float db = 10 * log10f (acc * s->accumulator_scale);
		if (db > 0)
			db = 0;

		// Assuming decibels are always negative (i.e., properly normalized).
		// The division defines the cutoff: 9 * 7 = 63 dB of range.
		int height = N_ELEMENTS (spectrum_bars) - 1 + (int) (db / 7);
		p += strlen (strcpy (p, spectrum_bars[MAX (height, 0)]));
	}
}

static bool
spectrum_init (struct spectrum *s, char *format, int bars, struct error **e)
{
	errno = 0;

	long sampling_rate, bits, channels;
	if (!format
	 || (sampling_rate = strtol (format, &format, 10), *format++ != ':')
	 || (bits          = strtol (format, &format, 10), *format++ != ':')
	 || (channels      = strtol (format, &format, 10), *format)
	 || errno != 0)
		return error_set (e, "invalid format, expected RATE:BITS:CHANNELS");

	if (sampling_rate < 20000 || sampling_rate > INT_MAX)
		return error_set (e, "unsupported sampling rate (%ld)", sampling_rate);
	if (bits != 8 && bits != 16)
		return error_set (e, "unsupported bit count (%ld)", bits);
	if (channels < 1 || channels > INT_MAX)
		return error_set (e, "no channels to sample (%ld)", channels);
	if (bars < 1 || bars > 12)
		return error_set (e, "requested too few or too many bars (%d)", bars);

	// All that can fail henceforth is memory allocation
	*s = (struct spectrum)
	{
		.sampling_rate = sampling_rate,
		.bits          = bits,
		.channels      = channels,
		.bars          = bars,
	};

	// The number of bars is always smaller than that of the samples (bins).
	// Let's start with the equation of the top FFT bin to use for a given bar:
	//   top_bin = (num_bins + 1) ^ (bar / num_bars) - 1
	// N.b. if we didn't subtract, the power function would make this ≥ 1.
	// N.b. we then also need to extend the range by the same amount.
	//
	// We need the amount of bins for the first bar to be at least one:
	//         1 ≤ (num_bins + 1) ^   (1 / num_bars) - 1
	//
	// Solving with Wolfram Alpha gives us:
	//   num_bins ≥ (2 ^ num_bars) - 1  [for y > 0]
	//
	// And we need to remember that half of the FFT bins are useless/missing--
	// FFTW skips useless points past the Nyquist frequency.
	int necessary_bins = 2 << s->bars;

	// Discard frequencies above 20 kHz, which take up a constant ratio
	// of all bins, given by the sampling rate.  A more practical/efficient
	// solution would be to just handle 96/192/... kHz rates as bitshifts.
	//
	// Filtering out sub-20 Hz frequencies would be even more wasteful than
	// this wild DFT size, so we don't even try.  While we may just shift
	// the lowest used bin easily within the extra range provided by this
	// extension (the Nyquist is usually above 22 kHz, and it hardly matters
	// if we go a bit beyond 20 kHz in the last bin), for a small number of bars
	// the first bin already includes audible frequencies, and even for larger
	// numbers it wouldn't be too accurate.  An exact solution would require
	// having the amount of bins be strictly a factor of Nyquist / 20 (stemming
	// from the equation 20 = Nyquist / bins).  Since log2(44100 / 2 / 20) > 10,
	// it would be fairly expensive, and somewhat slowly updating.  Always.
	// (Note that you can increase window overlap to get smoother framerates,
	// but it would remain laggy.)
	double audible_ratio = s->sampling_rate / 2. / 20000;
	s->bins = ceil (necessary_bins * MAX (audible_ratio, 1));
	s->useful_bins = s->bins / 2;

	int used_bins = necessary_bins / 2;
	s->spectrum = xcalloc (sizeof *s->spectrum, s->bars * 3 + 1);
	s->top_bins = xcalloc (sizeof *s->top_bins, s->bars);
	for (int bar = 0; bar < s->bars; bar++)
	{
		int top_bin = floor (pow (used_bins + 1, (bar + 1.) / s->bars)) - 1;
		s->top_bins[bar] = MIN (top_bin, used_bins);
	}

	// Limit updates to 30 times per second to limit CPU load
	s->samples = s->sampling_rate / s->bins * 2 / 30;
	if (s->samples < 1)
		s->samples = 1;

	// XXX: we average the channels but might want to average the DFT results
	if (s->bits == 8)   s->decode = spectrum_decode_8;
	if (s->bits == 16)  s->decode = spectrum_decode_16;

	// Micro-optimize to achieve some piece of mind; it's weak but measurable
	if (s->bits == 16 && s->channels == 2)
		s->decode = spectrum_decode_16_2;

	s->buffer_size = s->samples * s->useful_bins * s->bits / 8 * s->channels;
	s->buffer = xcalloc (1, s->buffer_size);

	// Prepare the window
	s->window = xcalloc (sizeof *s->window, s->bins);
	window_hann (s->window, s->bins);

	// Multiply by 2 for only using half of the DFT's result, then adjust to
	// the total energy of the window.  Both squared, because the accumulator
	// contains squared values.  Compute the average, and convert to decibels.
	// See also the mildly confusing https://dsp.stackexchange.com/a/14945.
	float coherent_gain = window_coherent_gain (s->window, s->bins);
	s->accumulator_scale = 2 * 2 / coherent_gain / coherent_gain / s->samples;

	s->data = xcalloc (sizeof *s->data, s->bins);
	s->windowed = fftw_malloc (sizeof *s->windowed * s->bins);
	s->out = fftw_malloc (sizeof *s->out * (s->useful_bins + 1));
	s->p = fftwf_plan_dft_r2c_1d (s->bins, s->windowed, s->out, FFTW_MEASURE);
	s->accumulator = xcalloc (sizeof *s->accumulator, s->useful_bins);
	return true;
}

static void
spectrum_free (struct spectrum *s)
{
	free (s->accumulator);
	fftwf_destroy_plan (s->p);
	fftw_free (s->out);
	fftw_free (s->windowed);
	free (s->data);
	free (s->window);

	free (s->spectrum);
	free (s->top_bins);
	free (s->buffer);

	memset (s, 0, sizeof *s);
}

#endif  // WITH_FFTW

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
	int item_mark;                      ///< Multiselect second point index
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
	int64_t elapsed_since;              ///< Last tick ts or last elapsed time
	bool elapsed_poll;                  ///< Poll MPD for the elapsed time?

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
	struct strv enqueue;                ///< Items to enqueue once connected

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

#ifdef WITH_FFTW
	struct spectrum spectrum;           ///< Spectrum analyser
	int spectrum_fd;                    ///< FIFO file descriptor (non-blocking)
	int spectrum_column, spectrum_row;  ///< Position for fast refresh
	struct poller_fd spectrum_event;    ///< FIFO watcher
#endif  // WITH_FFTW

	struct line_editor editor;          ///< Line editor
	struct poller_idle refresh_event;   ///< Refresh the screen

	// Terminal:

	termo_t *tk;                        ///< termo handle
	struct poller_timer tk_timer;       ///< termo timeout timer
	bool locale_is_utf8;                ///< The locale is Unicode
	bool use_partial_boxes;             ///< Use Unicode box drawing chars
	bool focused;                       ///< Whether the terminal has focus

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
	self->item_mark = -1;
}

static void
tab_free (struct tab *self)
{
	free (self->name);
}

static struct tab_range { int from, upto; }
tab_selection_range (struct tab *self)
{
	if (self->item_selected < 0 || !self->item_count)
		return (struct tab_range) { -1, -1 };
	if (self->item_mark < 0)
		return (struct tab_range) { self->item_selected, self->item_selected };
	return (struct tab_range) { MIN (self->item_selected, self->item_mark),
		MAX (self->item_selected, self->item_mark) };
}

// --- Configuration -----------------------------------------------------------

static void
on_poll_elapsed_time_changed (struct config_item *item)
{
	// This is only set once, on application startup
	g.elapsed_poll = item->value.boolean;
}

static struct config_schema g_config_settings[] =
{
	{ .name      = "address",
	  .comment   = "Address to connect to the MPD server",
	  .type      = CONFIG_ITEM_STRING,
	  .default_  = "\"localhost\"" },
	{ .name      = "password",
	  .comment   = "Password to use for MPD authentication",
	  .type      = CONFIG_ITEM_STRING },

	// NOTE: this is unused--in theory we could allow manual metadata adjustment
	// NOTE: the "config" command may return "music_directory" for local clients
	{ .name      = "root",
	  .comment   = "Where all the files MPD is playing are located",
	  .type      = CONFIG_ITEM_STRING },

#ifdef WITH_FFTW
	{ .name      = "spectrum_path",
	  .comment   = "Visualizer feed path to a FIFO audio output",
	  .type      = CONFIG_ITEM_STRING },
	// MPD's "outputs" command doesn't include this information
	{ .name      = "spectrum_format",
	  .comment   = "Visualizer feed data format",
	  .type      = CONFIG_ITEM_STRING,
	  .default_  = "\"44100:16:2\"" },
	// 10 is about the useful limit, then it gets too computationally expensive
	{ .name      = "spectrum_bars",
	  .comment   = "Number of computed audio spectrum bars",
	  .type      = CONFIG_ITEM_INTEGER,
	  .default_  = "8" },
#endif  // WITH_FFTW

	// Disabling this minimises MPD traffic and has the following caveats:
	//  - when MPD stalls on retrieving audio data, we keep ticking
	//  - when the "play" succeeds in ACTION_MPD_REPLACE for the same item as
	//    is currently playing, we do not reset g.song_elapsed (we could ask
	//    for a response which feels racy, or rethink the mechanism there)
	{ .name      = "poll_elapsed_time",
	  .comment   = "Whether to actively poll MPD for the elapsed time",
	  .type      = CONFIG_ITEM_BOOLEAN,
	  .on_change = on_poll_elapsed_time_changed,
	  .default_  = "on" },
	{}
};

static struct config_schema g_config_colors[] =
{
#define XX(name_, config, fg_, bg_, attrs_) \
	{ .name = #config, .type = CONFIG_ITEM_STRING },
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
	if ((value = get_config_string (subtree, #config))) \
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
	struct str_map_iter iter = str_map_iter_make (&subtree->value.object);
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
	g.client = mpd_client_make (&g.poller);
	g.config = config_make ();
	g.streams = strv_make ();
	g.enqueue = strv_make ();
	g.playlist = item_list_make ();

	g.playback_info = str_map_make (free);
	g.playback_info.key_xfrm = tolower_ascii_strxfrm;

#ifdef WITH_FFTW
	g.spectrum_fd = -1;
	g.spectrum_row = g.spectrum_column = -1;
#endif  // WITH_FFTW

	// This is also approximately what libunistring does internally,
	// since the locale name is canonicalized by locale_charset().
	// Note that non-Unicode locales are handled pretty inefficiently.
	g.locale_is_utf8 = !strcasecmp_ascii (locale_charset (), "UTF-8");

	// It doesn't work 100% (e.g. incompatible with undelining in urxvt)
	// TODO: make this configurable
	g.use_partial_boxes = g.locale_is_utf8;

	// Presumably, although not necessarily; unsure if queryable at all
	g.focused = true;

	app_init_attributes ();
}

static void
app_init_terminal (void)
{
	TERMO_CHECK_VERSION;
	if (!(g.tk = termo_new (STDIN_FILENO, NULL, 0)))
		exit_fatal ("failed to set up the terminal");
	if (!initscr () || nonl () == ERR)
		exit_fatal ("failed to set up the terminal");

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
	strv_free (&g.enqueue);
	item_list_free (&g.playlist);

#ifdef WITH_FFTW
	spectrum_free (&g.spectrum);
	if (g.spectrum_fd != -1)
	{
		poller_fd_reset (&g.spectrum_event);
		xclose (g.spectrum_fd);
	}
#endif  // WITH_FFTW

	line_editor_free (&g.editor);

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
	struct row_buffer buf = row_buffer_make ();
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
		struct row_buffer buf = row_buffer_make ();
		row_buffer_append (&buf, title, attr_highlight);
		app_flush_header (&buf, attr_normal);
	}

	char *artist = compact_map_find (map, "artist");
	char *album  = compact_map_find (map, "album");
	if (!artist && !album)
		return;

	struct row_buffer buf = row_buffer_make ();
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

	struct str s = str_make ();
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

	struct row_buffer buf = row_buffer_make ();
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

	// The help tab is disguised so that it's not too intruding
	struct row_buffer buf = row_buffer_make ();
	row_buffer_append (&buf, APP_TITLE, attrs[g.active_tab == g.help_tab]);
	row_buffer_append (&buf, " ", attrs[false]);

	g.tabs_offset = g.header_height;
	LIST_FOR_EACH (struct tab, iter, g.tabs)
		row_buffer_append (&buf, iter->name, attrs[iter == g.active_tab]);

#ifdef WITH_FFTW
	// This seems like the most reasonable, otherwise unoccupied space
	if (g.spectrum_fd != -1)
	{
		// Find some space and remember where it was, for fast refreshes
		row_buffer_ellipsis (&buf, COLS - g.spectrum.bars - 1);
		row_buffer_align (&buf, COLS - g.spectrum.bars, attrs[false]);
		g.spectrum_row = g.header_height;
		g.spectrum_column = buf.total_width;

		row_buffer_append (&buf, g.spectrum.spectrum, attrs[false]);
	}
#endif  // WITH_FFTW

	app_flush_header (&buf, attrs[false]);

	const char *header = g.active_tab->header;
	if (header)
	{
		buf = row_buffer_make ();
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

/// Figure out scrollbar appearance.  @a s is the minimal slider length as well
/// as the scrollbar resolution per @a visible item.
struct scrollbar { long length, start; }
app_compute_scrollbar (struct tab *tab, long visible, long s)
{
	long top = tab->item_top, total = tab->item_count;
	if (total < visible)
		return (struct scrollbar) { 0, 0 };
	if (visible == 1)
		return (struct scrollbar) { s, 0 };
	if (visible == 2)
		return (struct scrollbar) { s, top >= total / 2 ? s : 0 };

	// Only be at the top or bottom when the top or bottom item can be seen.
	// The algorithm isn't optimal but it's a bitch to get right.
	double available_length = s * visible - 2 - s + 1;

	double lenf = s + available_length * visible / total, length = 0.;
	long offset = 1 + available_length * top / total + modf (lenf, &length);

	if (top == 0)
		return (struct scrollbar) { length, 0 };
	if (top + visible >= total)
		return (struct scrollbar) { length, s * visible - length };

	return (struct scrollbar) { length, offset };
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

	hard_assert (tab->item_count != 0);
	if (!g.use_partial_boxes)
	{
		struct scrollbar bar = app_compute_scrollbar (tab, visible_items, 1);
		for (int row = 0; row < visible_items; row++)
		{
			move (g.header_height + row, COLS - 1);
			if (row < bar.start || row >= bar.start + bar.length)
				addch (' ' | APP_ATTR (SCROLLBAR));
			else
				addch (' ' | APP_ATTR (SCROLLBAR) | A_REVERSE);
		}
		return;
	}

	struct scrollbar bar = app_compute_scrollbar (tab, visible_items, 8);
	bar.length += bar.start;

	int start_part = bar.start  % 8; bar.start  /= 8;
	int end_part   = bar.length % 8; bar.length /= 8;

	// Even with this, the solid part must be at least one character high
	static const char *partials[] = { "█", "▇", "▆", "▅", "▄", "▃", "▂", "▁" };

	for (int row = 0; row < visible_items; row++)
	{
		chtype attrs = APP_ATTR (SCROLLBAR);
		if (row > bar.start && row <= bar.length)
			attrs ^= A_REVERSE;

		const char *c = " ";
		if (row == bar.start)  c = partials[start_part];
		if (row == bar.length) c = partials[end_part];

		move (g.header_height + row, COLS - 1);

		struct row_buffer buf = row_buffer_make ();
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

		bool override_colors = true;
		if (item_index == tab->item_selected)
			row_attrs = g.focused
				? APP_ATTR (SELECTION) : APP_ATTR (DEFOCUSED);
		else if (tab->item_mark > -1 &&
		   ((item_index >= tab->item_mark && item_index <= tab->item_selected)
		 || (item_index >= tab->item_selected && item_index <= tab->item_mark)))
			row_attrs = g.focused
				? APP_ATTR (MULTISELECT) : APP_ATTR (DEFOCUSED);
		else
			override_colors = false;

		struct row_buffer buf = row_buffer_make ();
		tab->on_item_draw (item_index, &buf, view_width);

		// Combine attributes used by the handler with the defaults.
		// Avoiding attrset() because of row_buffer_flush().
		for (size_t i = 0; i < buf.chars_len; i++)
		{
			chtype *attrs = &buf.chars[i].attrs;
			if (override_colors)
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
	struct str stats = str_make ();
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
	if (g.active_tab->item_mark > -1)
	{
		struct tab_range r = tab_selection_range (g.active_tab);
		char *msg = xstrdup_printf (r.from == r.upto
			? "Selected %d item" : "Selected %d items", r.upto - r.from + 1);
		row_buffer_append (buf, msg, APP_ATTR (HIGHLIGHT));
		free (msg);
	}
	else if (str_map_find (map, "updating_db"))
		row_buffer_append (buf, "Updating database...", APP_ATTR (NORMAL));
	else
		app_write_mpd_status_playlist (buf);

	const char *s;
	bool repeat  = (s = str_map_find (map, "repeat"))  && strcmp (s, "0");
	bool random  = (s = str_map_find (map, "random"))  && strcmp (s, "0");
	bool single  = (s = str_map_find (map, "single"))  && strcmp (s, "0");
	bool consume = (s = str_map_find (map, "consume")) && strcmp (s, "0");

	struct row_buffer right = row_buffer_make ();
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
	int caret = -1;

	struct row_buffer buf = row_buffer_make ();
	if (g.message)
		row_buffer_append (&buf, g.message, APP_ATTR (HIGHLIGHT));
	else if (g.editor.line)
		caret = line_editor_write (&g.editor, &buf, COLS, APP_ATTR (HIGHLIGHT));
	else if (g.client.state == MPD_CONNECTED)
		app_write_mpd_status (&buf);

	move (LINES - 1, 0);
	app_flush_buffer (&buf, COLS, APP_ATTR (NORMAL));

	curs_set (0);
	if (caret != -1)
	{
		move (LINES - 1, caret);
		curs_set (1);
	}
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

// --- Actions -----------------------------------------------------------------

#define ACTIONS(XX) \
	XX( NONE,               Do nothing                  ) \
	\
	XX( QUIT,               Quit                        ) \
	XX( REDRAW,             Redraw screen               ) \
	XX( TAB_HELP,           Switch to help tab          ) \
	XX( TAB_LAST,           Switch to last tab          ) \
	XX( TAB_PREVIOUS,       Switch to previous tab      ) \
	XX( TAB_NEXT,           Switch to next tab          ) \
	\
	XX( MPD_TOGGLE,         Toggle play/pause           ) \
	XX( MPD_STOP,           Stop playback               ) \
	XX( MPD_PREVIOUS,       Previous song               ) \
	XX( MPD_NEXT,           Next song                   ) \
	XX( MPD_BACKWARD,       Seek backwards              ) \
	XX( MPD_FORWARD,        Seek forwards               ) \
	XX( MPD_VOLUME_UP,      Increase volume             ) \
	XX( MPD_VOLUME_DOWN,    Decrease volume             ) \
	\
	XX( MPD_SEARCH,         Global search               ) \
	XX( MPD_ADD,            Add selection to playlist   ) \
	XX( MPD_REPLACE,        Replace playlist            ) \
	XX( MPD_REPEAT,         Toggle repeat               ) \
	XX( MPD_RANDOM,         Toggle random playback      ) \
	XX( MPD_SINGLE,         Toggle single song playback ) \
	XX( MPD_CONSUME,        Toggle consume              ) \
	XX( MPD_UPDATE_DB,      Update MPD database         ) \
	XX( MPD_COMMAND,        Send raw command to MPD     ) \
	\
	XX( CHOOSE,             Choose item                 ) \
	XX( DELETE,             Delete item                 ) \
	XX( UP,                 Go up a level               ) \
	XX( MULTISELECT,        Toggle multiselect          ) \
	\
	XX( SCROLL_UP,          Scroll up                   ) \
	XX( SCROLL_DOWN,        Scroll down                 ) \
	XX( MOVE_UP,            Move selection up           ) \
	XX( MOVE_DOWN,          Move selection down         ) \
	\
	XX( GOTO_TOP,           Go to top                   ) \
	XX( GOTO_BOTTOM,        Go to bottom                ) \
	XX( GOTO_ITEM_PREVIOUS, Go to previous item         ) \
	XX( GOTO_ITEM_NEXT,     Go to next item             ) \
	XX( GOTO_PAGE_PREVIOUS, Go to previous page         ) \
	XX( GOTO_PAGE_NEXT,     Go to next page             ) \
	\
	XX( GOTO_VIEW_TOP,      Select top item             ) \
	XX( GOTO_VIEW_CENTER,   Select center item          ) \
	XX( GOTO_VIEW_BOTTOM,   Select bottom item          ) \
	\
	XX( EDITOR_CONFIRM,     Confirm input               ) \
	\
	XX( EDITOR_B_CHAR,      Go back a character         ) \
	XX( EDITOR_F_CHAR,      Go forward a character      ) \
	XX( EDITOR_B_WORD,      Go back a word              ) \
	XX( EDITOR_F_WORD,      Go forward a word           ) \
	XX( EDITOR_HOME,        Go to start of line         ) \
	XX( EDITOR_END,         Go to end of line           ) \
	\
	XX( EDITOR_B_DELETE,    Delete last character       ) \
	XX( EDITOR_F_DELETE,    Delete next character       ) \
	XX( EDITOR_B_KILL_WORD, Delete last word            ) \
	XX( EDITOR_B_KILL_LINE, Delete everything up to BOL ) \
	XX( EDITOR_F_KILL_LINE, Delete everything up to EOL )

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
#define XX(name, description) { #name, #description },
	ACTIONS (XX)
#undef XX
};

/// Accept a more human format of action-name instead of ACTION_NAME
static int action_toupper (int c) { return c == '-' ? '_' : toupper_ascii (c); }

static int
action_resolve (const char *name)
{
	const unsigned char *s = (const unsigned char *) name;
	for (int i = 0; i < ACTION_COUNT; i++)
	{
		const char *target = g_actions[i].name;
		for (size_t k = 0; action_toupper (s[k]) == target[k]; k++)
			if (!s[k] && !target[k])
				return i;
	}
	return -1;
}

// --- User input handling -----------------------------------------------------

static void
mpd_client_vsend_command (struct mpd_client *self, va_list ap)
{
	struct strv v = strv_make ();
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
app_setvol (int value)
{
	char *volume = xstrdup_printf ("%d", MAX (0, MIN (100, value)));
	bool result = g.volume >= 0 && MPD_SIMPLE ("setvol", volume);
	free (volume);
	return result;
}

static void
app_on_editor_end (bool confirmed)
{
	struct mpd_client *c = &g.client;
	if (!confirmed)
		return;

	size_t len;
	char *u8 = (char *) u32_to_u8 (g.editor.line, g.editor.len + 1, NULL, &len);
	mpd_client_send_command_raw (c, u8);
	free (u8);

	mpd_client_add_task (c, mpd_on_simple_response, NULL);
	mpd_client_idle (c, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
app_mpd_toggle (const char *name)
{
	const char *s = str_map_find (&g.playback_info, name);
	bool value = s && strcmp (s, "0");
	return MPD_SIMPLE (name, value ? "0" : "1");
}

static bool
app_process_action (enum action action)
{
	// First let the tab try to handle this
	struct tab *tab = g.active_tab;
	if (tab->on_action && tab->on_action (action))
	{
		app_invalidate ();
		return true;
	}

	switch (action)
	{
	case ACTION_NONE:
		return true;
	case ACTION_QUIT:
		// It is a pseudomode, avoid surprising the user
		if (tab->item_mark > -1)
		{
			tab->item_mark = -1;
			app_invalidate ();
			return true;
		}

		app_quit ();
		return true;
	case ACTION_REDRAW:
		clear ();
		app_invalidate ();
		return true;
	case ACTION_MPD_COMMAND:
		line_editor_start (&g.editor, ':');
		g.editor.on_end = app_on_editor_end;
		app_invalidate ();
		return true;
	default:
		return false;

	case ACTION_MULTISELECT:
		if (!tab->can_multiselect
		 || !tab->item_count || tab->item_selected < 0)
			return false;

		app_invalidate ();
		if (tab->item_mark > -1)
			tab->item_mark = -1;
		else
			tab->item_mark = tab->item_selected;
		return true;

	case ACTION_TAB_LAST:
		if (!g.last_tab)
			return false;
		app_switch_tab (g.last_tab);
		return true;
	case ACTION_TAB_HELP:
		app_switch_tab (g.help_tab);
		return true;
	case ACTION_TAB_PREVIOUS:
		if (g.active_tab == g.help_tab)
			return false;
		if (!g.active_tab->prev)
			app_switch_tab (g.help_tab);
		else
			app_switch_tab (g.active_tab->prev);
		return true;
	case ACTION_TAB_NEXT:
		if (g.active_tab == g.help_tab)
			app_switch_tab (g.tabs);
		else if (g.active_tab->next)
			app_switch_tab (g.active_tab->next);
		else
			return false;
		return true;

	case ACTION_MPD_TOGGLE:
		if (g.state == PLAYER_PLAYING)  return MPD_SIMPLE ("pause", "1");
		if (g.state == PLAYER_PAUSED)   return MPD_SIMPLE ("pause", "0");
		return MPD_SIMPLE ("play");
	case ACTION_MPD_STOP:               return MPD_SIMPLE ("stop");
	case ACTION_MPD_PREVIOUS:           return MPD_SIMPLE ("previous");
	case ACTION_MPD_NEXT:               return MPD_SIMPLE ("next");
	case ACTION_MPD_FORWARD:            return MPD_SIMPLE ("seekcur", "+10");
	case ACTION_MPD_BACKWARD:           return MPD_SIMPLE ("seekcur", "-10");
	case ACTION_MPD_REPEAT:             return app_mpd_toggle ("repeat");
	case ACTION_MPD_RANDOM:             return app_mpd_toggle ("random");
	case ACTION_MPD_SINGLE:             return app_mpd_toggle ("single");
	case ACTION_MPD_CONSUME:            return app_mpd_toggle ("consume");
	case ACTION_MPD_UPDATE_DB:          return MPD_SIMPLE ("update");

	case ACTION_MPD_VOLUME_UP:          return app_setvol (g.volume + 10);
	case ACTION_MPD_VOLUME_DOWN:        return app_setvol (g.volume - 10);

		// XXX: these should rather be parametrized
	case ACTION_SCROLL_UP:              return app_scroll (-3);
	case ACTION_SCROLL_DOWN:            return app_scroll  (3);

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

	case ACTION_GOTO_ITEM_PREVIOUS:     return app_move_selection (-1);
	case ACTION_GOTO_ITEM_NEXT:         return app_move_selection (1);

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
	}
	return false;
}

static bool
app_editor_process_action (enum action action)
{
	app_invalidate ();
	switch (action)
	{
	case ACTION_QUIT:
		line_editor_abort (&g.editor, false);
		g.editor.on_end = NULL;
		return true;
	case ACTION_EDITOR_CONFIRM:
		line_editor_abort (&g.editor, true);
		g.editor.on_end = NULL;
		return true;
	default:
		return false;

	case ACTION_EDITOR_B_CHAR:
		return line_editor_action (&g.editor, LINE_EDITOR_B_CHAR);
	case ACTION_EDITOR_F_CHAR:
		return line_editor_action (&g.editor, LINE_EDITOR_F_CHAR);
	case ACTION_EDITOR_B_WORD:
		return line_editor_action (&g.editor, LINE_EDITOR_B_WORD);
	case ACTION_EDITOR_F_WORD:
		return line_editor_action (&g.editor, LINE_EDITOR_F_WORD);
	case ACTION_EDITOR_HOME:
		return line_editor_action (&g.editor, LINE_EDITOR_HOME);
	case ACTION_EDITOR_END:
		return line_editor_action (&g.editor, LINE_EDITOR_END);

	case ACTION_EDITOR_B_DELETE:
		return line_editor_action (&g.editor, LINE_EDITOR_B_DELETE);
	case ACTION_EDITOR_F_DELETE:
		return line_editor_action (&g.editor, LINE_EDITOR_F_DELETE);
	case ACTION_EDITOR_B_KILL_WORD:
		return line_editor_action (&g.editor, LINE_EDITOR_B_KILL_WORD);
	case ACTION_EDITOR_B_KILL_LINE:
		return line_editor_action (&g.editor, LINE_EDITOR_B_KILL_LINE);
	case ACTION_EDITOR_F_KILL_LINE:
		return line_editor_action (&g.editor, LINE_EDITOR_F_KILL_LINE);
	}
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

	if (g.editor.line)
		line_editor_abort (&g.editor, false);

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
	termo_key_t decoded;                ///< Decoded key definition
	enum action action;                 ///< Action to take
	int order;                          ///< Order for stable sorting
}
*g_normal_keys, *g_editor_keys;
static size_t g_normal_keys_len, g_editor_keys_len;

static struct binding_default
{
	const char *key;                    ///< Key definition
	enum action action;                 ///< Action to take
}
g_normal_defaults[] =
{
	{ "Escape",     ACTION_QUIT               },
	{ "q",          ACTION_QUIT               },
	{ "C-l",        ACTION_REDRAW             },
	{ "M-Tab",      ACTION_TAB_LAST           },
	{ "F1",         ACTION_TAB_HELP           },
	{ "C-Left",     ACTION_TAB_PREVIOUS       },
	{ "C-Right",    ACTION_TAB_NEXT           },
	{ "C-PageUp",   ACTION_TAB_PREVIOUS       },
	{ "C-PageDown", ACTION_TAB_NEXT           },

	{ "Home",       ACTION_GOTO_TOP           },
	{ "End",        ACTION_GOTO_BOTTOM        },
	{ "M-<",        ACTION_GOTO_TOP           },
	{ "M->",        ACTION_GOTO_BOTTOM        },
	{ "g",          ACTION_GOTO_TOP           },
	{ "G",          ACTION_GOTO_BOTTOM        },
	{ "S-Up",       ACTION_MOVE_UP            },
	{ "S-Down",     ACTION_MOVE_DOWN          },
	{ "Up",         ACTION_GOTO_ITEM_PREVIOUS },
	{ "Down",       ACTION_GOTO_ITEM_NEXT     },
	{ "k",          ACTION_GOTO_ITEM_PREVIOUS },
	{ "j",          ACTION_GOTO_ITEM_NEXT     },
	{ "PageUp",     ACTION_GOTO_PAGE_PREVIOUS },
	{ "PageDown",   ACTION_GOTO_PAGE_NEXT     },
	{ "C-p",        ACTION_GOTO_ITEM_PREVIOUS },
	{ "C-n",        ACTION_GOTO_ITEM_NEXT     },
	{ "C-b",        ACTION_GOTO_PAGE_PREVIOUS },
	{ "C-f",        ACTION_GOTO_PAGE_NEXT     },
	{ "C-y",        ACTION_SCROLL_UP          },
	{ "C-e",        ACTION_SCROLL_DOWN        },

	{ "H",          ACTION_GOTO_VIEW_TOP      },
	{ "M",          ACTION_GOTO_VIEW_CENTER   },
	{ "L",          ACTION_GOTO_VIEW_BOTTOM   },

	// Not sure how to set these up, they're pretty arbitrary so far
	{ "Enter",      ACTION_CHOOSE             },
	{ "Delete",     ACTION_DELETE             },
	{ "d",          ACTION_DELETE             },
	{ "M-Up",       ACTION_UP                 },
	{ "Backspace",  ACTION_UP                 },
	{ "v",          ACTION_MULTISELECT        },
	{ "/",          ACTION_MPD_SEARCH         },
	{ "a",          ACTION_MPD_ADD            },
	{ "r",          ACTION_MPD_REPLACE        },
	{ ":",          ACTION_MPD_COMMAND        },

	{ "<",          ACTION_MPD_PREVIOUS       },
	{ ">",          ACTION_MPD_NEXT           },
	{ "Left",       ACTION_MPD_PREVIOUS       },
	{ "Right",      ACTION_MPD_NEXT           },
	{ "M-Left",     ACTION_MPD_BACKWARD       },
	{ "M-Right",    ACTION_MPD_FORWARD        },
	{ "h",          ACTION_MPD_PREVIOUS       },
	{ "l",          ACTION_MPD_NEXT           },
	{ "Space",      ACTION_MPD_TOGGLE         },
	{ "C-Space",    ACTION_MPD_STOP           },
	{ "u",          ACTION_MPD_UPDATE_DB      },
	{ "M-PageUp",   ACTION_MPD_VOLUME_UP      },
	{ "M-PageDown", ACTION_MPD_VOLUME_DOWN    },
},
g_editor_defaults[] =
{
	{ "Left",       ACTION_EDITOR_B_CHAR      },
	{ "Right",      ACTION_EDITOR_F_CHAR      },
	{ "C-b",        ACTION_EDITOR_B_CHAR      },
	{ "C-f",        ACTION_EDITOR_F_CHAR      },
	{ "M-b",        ACTION_EDITOR_B_WORD      },
	{ "M-f",        ACTION_EDITOR_F_WORD      },
	{ "Home",       ACTION_EDITOR_HOME        },
	{ "End",        ACTION_EDITOR_END         },
	{ "C-a",        ACTION_EDITOR_HOME        },
	{ "C-e",        ACTION_EDITOR_END         },

	{ "C-h",        ACTION_EDITOR_B_DELETE    },
	{ "DEL",        ACTION_EDITOR_B_DELETE    },
	{ "Backspace",  ACTION_EDITOR_B_DELETE    },
	{ "C-d",        ACTION_EDITOR_F_DELETE    },
	{ "Delete",     ACTION_EDITOR_F_DELETE    },
	{ "C-u",        ACTION_EDITOR_B_KILL_LINE },
	{ "C-k",        ACTION_EDITOR_F_KILL_LINE },
	{ "C-w",        ACTION_EDITOR_B_KILL_WORD },

	{ "C-g",        ACTION_QUIT               },
	{ "Escape",     ACTION_QUIT               },
	{ "Enter",      ACTION_EDITOR_CONFIRM     },
};

static int
app_binding_cmp (const void *a, const void *b)
{
	const struct binding *aa = a, *bb = b;
	int cmp = termo_keycmp (g.tk, &aa->decoded, &bb->decoded);
	return cmp ? cmp : bb->order - aa->order;
}

static bool
app_next_binding (struct str_map_iter *iter, termo_key_t *key, int *action)
{
	struct config_item *v;
	while ((v = str_map_iter_next (iter)))
	{
		*action = ACTION_NONE;
		if (*termo_strpkey_utf8 (g.tk,
			iter->link->key, key, TERMO_FORMAT_ALTISMETA))
			print_error ("%s: invalid binding", iter->link->key);
		else if (v->type == CONFIG_ITEM_NULL)
			return true;
		else if (v->type != CONFIG_ITEM_STRING)
			print_error ("%s: bindings must be strings", iter->link->key);
		else if ((*action = action_resolve (v->value.string.str)) >= 0)
			return true;
		else
			print_error ("%s: unknown action: %s",
				iter->link->key, v->value.string.str);
	}
	return false;
}

static struct binding *
app_init_bindings (const char *keymap,
	struct binding_default *defaults, size_t defaults_len, size_t *result_len)
{
	ARRAY (struct binding, a)
	ARRAY_INIT_SIZED (a, defaults_len);

	// Order for stable sorting
	size_t order = 0;

	termo_key_t decoded;
	for (size_t i = 0; i < defaults_len; i++)
	{
		hard_assert (!*termo_strpkey_utf8 (g.tk,
			defaults[i].key, &decoded, TERMO_FORMAT_ALTISMETA));
		a[a_len++] = (struct binding) { decoded, defaults[i].action, order++ };
	}

	struct config_item *root = config_item_get (g.config.root, keymap, NULL);
	if (root && root->type == CONFIG_ITEM_OBJECT)
	{
		struct str_map_iter iter = str_map_iter_make (&root->value.object);
		ARRAY_RESERVE (a, iter.map->len);

		int action;
		while (app_next_binding (&iter, &decoded, &action))
			a[a_len++] = (struct binding) { decoded, action, order++ };
	}

	// Use the helper field to use the last mappings of identical bindings
	size_t out = 0;
	qsort (a, a_len, sizeof *a, app_binding_cmp);
	for (size_t in = 0; in < a_len; in++)
	{
		a[in].order = 0;
		if (!out || termo_keycmp (g.tk, &a[in].decoded, &a[out - 1].decoded))
			a[out++] = a[in];
	}

	*result_len = out;
	return a;
}

static bool
app_process_termo_event (termo_key_t *event)
{
	bool handled = false;
	if ((handled = event->type == TERMO_TYPE_FOCUS))
	{
		g.focused = !!event->code.focused;
		app_invalidate ();
		// Senseless fall-through
	}

	struct binding dummy = { *event, 0, 0 }, *binding;
	if (g.editor.line)
	{
		if ((binding = bsearch (&dummy, g_editor_keys, g_editor_keys_len,
			sizeof *binding, app_binding_cmp)))
			return app_editor_process_action (binding->action);
		if (event->type != TERMO_TYPE_KEY || event->modifiers != 0)
			return handled;

		line_editor_insert (&g.editor, event->code.codepoint);
		app_invalidate ();
		return true;
	}
	if ((binding = bsearch (&dummy, g_normal_keys, g_normal_keys_len,
		sizeof *binding, app_binding_cmp)))
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
	return handled;
}

// --- Current tab -------------------------------------------------------------

static struct tab g_current_tab;

#define DURATION_MAX_LEN (1 /*separator */ + 2 /* h */ + 3 /* m */+ 3 /* s */)

static void
current_tab_on_item_draw (size_t item_index, struct row_buffer *buffer,
	int width)
{
	// TODO: configurable output, maybe dynamically sized columns
	compact_map_t map = item_list_get (&g.playlist, item_index);
	const char *artist = compact_map_find (map, "artist");
	const char *title  = compact_map_find (map, "title");

	chtype attrs = (int) item_index == g.song ? A_BOLD : 0;
	if (artist && title)
		row_buffer_append_args (buffer,
			artist, attrs, " - ", attrs, title, attrs, NULL);
	else
		row_buffer_append (buffer, compact_map_find (map, "file"), attrs);

	row_buffer_align (buffer, width - DURATION_MAX_LEN, attrs);

	int duration = -1;
	mpd_read_time (compact_map_find (map, "duration"), &duration, NULL);
	mpd_read_time (compact_map_find (map, "time"),     &duration, NULL);

	char *s = duration < 0 ? xstrdup ("-") : app_time_string (duration);
	char *right_aligned = xstrdup_printf ("%*s", DURATION_MAX_LEN, s);
	row_buffer_append (buffer, right_aligned, attrs);
	free (right_aligned);
	free (s);
}

static void
mpd_on_move_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) data;

	*(bool *) user_data = false;
	if (!response->success)
		print_error ("%s: %s", "command failed", response->message_text);
}

static void
current_tab_move (int from, int to)
{
	compact_map_t map;
	const char *id;
	if (!(map = item_list_get (&g.playlist, from))
	 || !(id = compact_map_find (map, "id")))
		return;

	char *target_str = xstrdup_printf ("%d", to);
	mpd_client_send_command (&g.client, "moveid", id, target_str, NULL);
	free (target_str);
}

static bool
current_tab_move_selection (int diff)
{
	static bool already_moving;
	if (already_moving || diff == 0)
		return true;

	struct mpd_client *c = &g.client;
	if (c->state != MPD_CONNECTED)
		return false;

	struct tab *tab = &g_current_tab;
	struct tab_range range = tab_selection_range (tab);
	if (range.from + diff < 0
	 || range.upto + diff >= (int) tab->item_count)
		return false;

	mpd_client_list_begin (c);
	if (diff < 0)
		for (int i = range.from; i <= range.upto; i++)
			current_tab_move (i, i + diff);
	else
		for (int i = range.upto; i >= range.from; i--)
			current_tab_move (i, i + diff);
	mpd_client_list_end (c);

	mpd_client_add_task (c, mpd_on_move_response, &already_moving);
	mpd_client_idle (c, 0);
	return already_moving = true;
}

static bool
current_tab_on_action (enum action action)
{
	struct tab *tab = &g_current_tab;
	compact_map_t map = item_list_get (&g.playlist, tab->item_selected);
	switch (action)
	{
		const char *id;
	case ACTION_MOVE_UP:
		return current_tab_move_selection (-1);
	case ACTION_MOVE_DOWN:
		return current_tab_move_selection (+1);
	case ACTION_CHOOSE:
		tab->item_mark = -1;
		return map && (id = compact_map_find (map, "id"))
			&& MPD_SIMPLE ("playid", id);
	case ACTION_DELETE:
	{
		struct mpd_client *c = &g.client;
		struct tab_range range = tab_selection_range (tab);
		if (range.from < 0 || c->state != MPD_CONNECTED)
			return false;

		mpd_client_list_begin (c);
		for (int i = range.from; i <= range.upto; i++)
		{
			if ((map = item_list_get (&g.playlist, i))
			 && (id = compact_map_find (map, "id")))
				mpd_client_send_command (c, "deleteid", id, NULL);
		}
		mpd_client_list_end (c);
		mpd_client_add_task (c, mpd_on_simple_response, NULL);
		mpd_client_idle (c, 0);
		return true;
	}
	default:
		return false;
	}
}

static void
current_tab_update (void)
{
	g_current_tab.item_count = g.playlist.len;
	g_current_tab.item_mark =
		MIN ((int) g.playlist.len - 1, g_current_tab.item_mark);
	app_invalidate ();
}

static struct tab *
current_tab_init (void)
{
	struct tab *super = &g_current_tab;
	tab_init (super, "Current");
	super->can_multiselect = true;
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

enum
{
	// This list is also ordered by ASCII and important for sorting

	LIBRARY_ROOT     = '/',             ///< Root entry
	LIBRARY_UP       = '^',             ///< Upper directory
	LIBRARY_DIR      = 'd',             ///< Directory
	LIBRARY_FILE     = 'f',             ///< File
	LIBRARY_PLAYLIST = 'p',             ///< Playlist (unsupported)
};

struct library_tab_item
{
	int type;                           ///< Type of the item
	int duration;                       ///< Duration or -1 if N/A or unknown
	char *name;                         ///< Visible name
	const char *path;                   ///< MPD path (follows the name)
};

static struct
{
	struct tab super;                   ///< Parent class
	struct str path;                    ///< Current path
	struct library_level *above;        ///< Upper levels

	/// Current items
	ARRAY (struct library_tab_item, items)

	bool searching;                     ///< Search mode is active
}
g_library_tab;

static void
library_tab_add (int type, int duration, const char *name, const char *path)
{
	// Slightly reduce memory overhead while retaining friendly access
	size_t name_len = strlen (name), path_len = strlen (path);
	char *combined = xmalloc (++name_len + ++path_len);

	ARRAY_RESERVE (g_library_tab.items, 1);
	g_library_tab.items[g_library_tab.items_len++] = (struct library_tab_item)
	{
		.type = type,
		.duration = duration,
		.name = memcpy (combined, name, name_len),
		.path = memcpy (combined + name_len, path, path_len),
	};
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
library_tab_on_item_draw (size_t item_index, struct row_buffer *buffer,
	int width)
{
	(void) width;
	hard_assert (item_index < g_library_tab.items_len);

	struct library_tab_item *x = &g_library_tab.items[item_index];
	const char *prefix, *name;
	switch (x->type)
	{
	case LIBRARY_ROOT: prefix = "/"; name = "";      break;
	case LIBRARY_UP:   prefix = "/"; name = "..";    break;
	case LIBRARY_DIR:  prefix = "/"; name = x->name; break;
	case LIBRARY_FILE: prefix = " "; name = x->name; break;
	default:           hard_assert (!"invalid item type");
	}
	chtype attrs = x->type != LIBRARY_FILE ? APP_ATTR (DIRECTORY) : 0;
	row_buffer_append_args (buffer, prefix, attrs, name, attrs, NULL);
	if (x->duration < 0)
		return;

	char *s = app_time_string (x->duration);
	row_buffer_align (buffer, width - 2 /* gap */ - strlen (s), 0);
	row_buffer_append_args (buffer, "  " /* gap */, 0, s, 0, NULL);
	free (s);
}

static char
library_tab_header_type (const char *key)
{
	if (!strcasecmp_ascii (key, "file"))      return LIBRARY_FILE;
	if (!strcasecmp_ascii (key, "directory")) return LIBRARY_DIR;
	if (!strcasecmp_ascii (key, "playlist"))  return LIBRARY_PLAYLIST;
	return 0;
}

static void
library_tab_chunk (char type, const char *path, struct str_map *map)
{
	// CUE files appear once as a directory and another time as a playlist,
	// just skip them entirely
	if (type == LIBRARY_PLAYLIST)
		return;

	const char *artist = str_map_find (map, "artist");
	const char *title  = str_map_find (map, "title");
	char *name = (artist && title)
		? xstrdup_printf ("%s - %s", artist, title)
		: xstrdup (xbasename (path));

	int duration = -1;
	mpd_read_time (str_map_find (map, "duration"), &duration, NULL);
	mpd_read_time (str_map_find (map, "time"),     &duration, NULL);
	library_tab_add (type, duration, name, path);
	free (name);
}

static int
library_tab_compare (struct library_tab_item *a, struct library_tab_item *b)
{
	if (a->type != b->type)
		return a->type - b->type;

	return app_casecmp ((uint8_t *) a->path, (uint8_t *) b->path);
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
library_tab_is_above (const char *above, const char *subdir)
{
	size_t above_len = strlen (above);
	if (strncmp (above, subdir, above_len))
		return false;
	// The root is an empty string and is above anything other than itself
	return subdir[above_len] == '/' || (*subdir && !*above);
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

	cstr_set (&g_library_tab.super.header, NULL);
	g_library_tab.super.item_mark = -1;

	if (path->len)
		g_library_tab.super.header = xstrdup_printf ("/%s", path->str);
}

static void
library_tab_reset (void)
{
	for (size_t i = 0; i < g_library_tab.items_len; i++)
		free (g_library_tab.items[i].name);
	free (g_library_tab.items);
	ARRAY_INIT (g_library_tab.items);
}

static void
library_tab_load_data (const struct strv *data)
{
	library_tab_reset ();

	char *parent = library_tab_parent ();
	if (parent)
	{
		library_tab_add (LIBRARY_ROOT, -1, "", "");
		library_tab_add (LIBRARY_UP, -1, "", parent);
		free (parent);
	}

	struct str_map map = str_map_make (NULL);
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

	struct library_tab_item *items = g_library_tab.items;
	size_t len = g_library_tab.super.item_count = g_library_tab.items_len;
	qsort (items, len, sizeof *items,
		(int (*) (const void *, const void *)) library_tab_compare);

	// XXX: this unmarks even if just the database updates
	g_library_tab.super.item_mark = -1;

	// Don't force the selection visible when there's no need to touch it
	if (g_library_tab.super.item_selected >= (int) len)
		app_move_selection (0);

	app_invalidate ();
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

	g_library_tab.searching = false;
	library_tab_change_level (new_path);
	free (new_path);

	library_tab_load_data (data);
}

static void
library_tab_reload (const char *new_path)
{
	if (!new_path && g_library_tab.searching)
		return;  // TODO: perhaps we should call search_on_changed()

	char *path = new_path
		? xstrdup (new_path)
		: xstrdup (g_library_tab.path.str);

	struct mpd_client *c = &g.client;
	mpd_client_send_command (c, "lsinfo", *path ? path : "/", NULL);
	mpd_client_add_task (c, library_tab_on_data, path);
	mpd_client_idle (c, 0);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
library_tab_on_search_data (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) user_data;
	if (!g_library_tab.searching)
		return;

	if (!response->success)
	{
		print_error ("cannot search: %s", response->message_text);
		return;
	}

	library_tab_load_data (data);
}

static void
search_on_changed (void)
{
	struct mpd_client *c = &g.client;

	size_t len;
	char *u8 = (char *) u32_to_u8 (g.editor.line, g.editor.len + 1, NULL, &len);
	mpd_client_send_command (c, "search", "any", u8, NULL);
	free (u8);

	mpd_client_add_task (c, library_tab_on_search_data, NULL);
	mpd_client_idle (c, 0);
}

static void
search_on_end (bool confirmed)
{
	if (!confirmed)
		library_tab_reload (g_library_tab.above->path);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
library_tab_is_range_playable (struct tab_range range)
{
	for (int i = range.from; i <= range.upto; i++)
	{
		struct library_tab_item *x = &g_library_tab.items[i];
		if (x->type == LIBRARY_DIR || x->type == LIBRARY_FILE)
			return true;
	}
	return false;
}

static bool
library_tab_on_action (enum action action)
{
	struct mpd_client *c = &g.client;
	if (c->state != MPD_CONNECTED)
		return false;

	struct tab *tab = &g_library_tab.super;
	struct tab_range range = tab_selection_range (tab);
	if (range.from < 0)
		return false;

	struct library_tab_item *x = &g_library_tab.items[range.from];
	switch (action)
	{
	case ACTION_CHOOSE:
		// I can't think of a reasonable way of handling that
		if (range.from != range.upto)
			break;

		switch (x->type)
		{
		case LIBRARY_ROOT:
		case LIBRARY_UP:
		case LIBRARY_DIR:  library_tab_reload (x->path); break;
		case LIBRARY_FILE: MPD_SIMPLE ("add", x->path);  break;
		default:           hard_assert (!"invalid item type");
		}
		tab->item_mark = -1;
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
	case ACTION_MPD_SEARCH:
	{
		line_editor_start (&g.editor, '/');
		g.editor.on_changed = search_on_changed;
		g.editor.on_end = search_on_end;

		// We just need to be deeper but not match anything real,
		// in order to keep the rest of the codebase functional as-is
		if (!g_library_tab.searching)
		{
			char *fake_subdir = xstrdup_printf ("%s/", g_library_tab.path.str);
			library_tab_change_level (fake_subdir);
			free (fake_subdir);
		}

		cstr_set (&tab->header, xstrdup_printf ("Global search"));
		g_library_tab.searching = true;

		// Since we've already changed the header, empty the list,
		// although to be consistent we should also ask to search for "",
		// which dumps the database
		struct strv empty = strv_make ();
		library_tab_load_data (&empty);
		strv_free (&empty);

		app_invalidate ();
		return true;
	}
	case ACTION_MPD_ADD:
		if (!library_tab_is_range_playable (range))
			break;

		for (int i = range.from; i <= range.upto; i++)
		{
			struct library_tab_item *x = &g_library_tab.items[i];
			if (x->type == LIBRARY_DIR || x->type == LIBRARY_FILE)
				MPD_SIMPLE ("add", x->path);
		}
		tab->item_mark = -1;
		return true;
	case ACTION_MPD_REPLACE:
		if (!library_tab_is_range_playable (range))
			break;

		// Clears the playlist (which stops playback), add what user wanted
		// to replace it with, and eventually restore playback;
		// I can't think of a reliable alternative that omits the "play"
		mpd_client_list_begin (c);

		mpd_client_send_command (c, "clear", NULL);
		for (int i = range.from; i <= range.upto; i++)
		{
			struct library_tab_item *x = &g_library_tab.items[i];
			if (x->type == LIBRARY_DIR || x->type == LIBRARY_FILE)
				mpd_client_send_command (c, "add", x->path, NULL);
		}
		if (g.state == PLAYER_PLAYING)
			mpd_client_send_command (c, "play", NULL);

		mpd_client_list_end (c);
		mpd_client_add_task (c, mpd_on_simple_response, NULL);
		mpd_client_idle (c, 0);
		tab->item_mark = -1;
		return true;
	default:
		break;
	}
	return false;
}

static struct tab *
library_tab_init (void)
{
	g_library_tab.path = str_make ();
	// g_library_tab.items is fine with zero initialisation

	struct tab *super = &g_library_tab.super;
	tab_init (super, "Library");
	super->can_multiselect = true;
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
streams_tab_filter (char *line, regex_t *re, struct strv *out)
{
	regmatch_t groups[2];
	if (regexec (re, line, 2, groups, 0) == REG_NOMATCH)
		return;

	// It may happen that playlist files contain useless, invalid quotes,
	// let's be liberal in what we accept
	regoff_t start = groups[1].rm_so, end = groups[1].rm_eo;
	while (end > start + 1 && line[start] == '"' && line[end - 1] == '"')
	{
		start++;
		end--;
	}

	char *target = xstrndup (line + start, end - start);
	if (utf8_validate (target, end - start))
		strv_append_owned (out, target);
	else
	{
		strv_append_owned (out, latin1_to_utf8 (target));
		free (target);
	}
}

static void
streams_tab_parse_playlist (const char *playlist, const char *content_type,
	struct strv *out)
{
	// We accept a lot of very broken stuff because this is the real world
	struct strv lines = strv_make ();
	cstr_split (playlist, "\r\n", true, &lines);

	// Since this excludes '"', it should even work for XMLs (w/o entities)
	const char *extract_re =
		"(https?://([][a-z0-9._~:/?#@!$&'()*+,;=-]|%[a-f0-9]{2})+)";
	if ((lines.len && !strcasecmp_ascii (lines.vector[0], "[playlist]"))
	 || (content_type && is_content_type (content_type, "audio", "x-scpls")))
		extract_re = "^File[^=]*=(.+)";
	else if ((lines.len && !strcasecmp_ascii (lines.vector[0], "#EXTM3U"))
	 || (content_type && is_content_type (content_type, "audio", "x-mpegurl")))
		extract_re = "^([^#].*)";

	regex_t *re = regex_compile (extract_re, REG_EXTENDED, NULL);
	hard_assert (re != NULL);
	for (size_t i = 0; i < lines.len; i++)
		streams_tab_filter (lines.vector[i], re, out);
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
		if (iscntrl_ascii (c) & (c != '\t') & (c != '\r') & (c != '\n'))
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

	struct strv links = strv_make ();
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
	task.data = str_make ();
	task.replace = replace;
	bool result = false;

	struct curl_slist *ok_headers = curl_slist_append (NULL, "ICY 200 OK");

	CURLcode res;
	if ((res = curl_easy_setopt (easy, CURLOPT_FOLLOWLOCATION, 1L))
	 || (res = curl_easy_setopt (easy, CURLOPT_NOPROGRESS,     1L))
	// TODO: make the timeout a bit larger once we're asynchronous
	 || (res = curl_easy_setopt (easy, CURLOPT_TIMEOUT,        5L))
	// Not checking anything, we just want some data, any data
	 || (res = curl_easy_setopt (easy, CURLOPT_SSL_VERIFYPEER, 0L))
	 || (res = curl_easy_setopt (easy, CURLOPT_SSL_VERIFYHOST, 0L))
	 || (res = curl_easy_setopt (easy, CURLOPT_URL,            uri))
	 || (res = curl_easy_setopt (easy, CURLOPT_HTTP200ALIASES, ok_headers))

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
	curl_slist_free_all (ok_headers);
	str_free (&task.data);
	poller_curl_free (&pc);

	poller_free (&poller);
	return result;
}

static bool
streams_tab_on_action (enum action action)
{
	struct tab *tab = g.active_tab;
	if (tab->item_selected < 0 || !tab->item_count)
		return false;

	// For simplicity the URL is the string following the stream name
	const char *uri = 1 + strchr (g.streams.vector[tab->item_selected], 0);

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
	g_info_tab.keys = strv_make ();
	g_info_tab.values = strv_make ();

	struct tab *super = &g_info_tab.super;
	tab_init (super, "Info");
	super->on_item_draw = info_tab_on_item_draw;
	return super;
}

// --- Help tab ----------------------------------------------------------------

static struct
{
	struct tab super;                   ///< Parent class
	ARRAY (enum action, actions)        ///< Actions for content
	struct strv lines;                  ///< Visible content
}
g_help_tab;

static bool
help_tab_on_action (enum action action)
{
	struct tab *tab = &g_help_tab.super;
	if (tab->item_selected < 0
	 || tab->item_selected >= (int) g_help_tab.actions_len
	 || action != ACTION_CHOOSE)
		return false;

	action = g_help_tab.actions[tab->item_selected];
	return action != ACTION_NONE
		&& action != ACTION_CHOOSE  // avoid recursion
		&& app_process_action (action);
}

static void
help_tab_strfkey (const termo_key_t *key, struct strv *out)
{
	// For display purposes, this is highly desirable
	int flags = termo_get_flags (g.tk);
	termo_set_flags (g.tk, flags | TERMO_FLAG_SPACESYMBOL);
	termo_key_t fixed = *key;
	termo_canonicalise (g.tk, &fixed);
	termo_set_flags (g.tk, flags);

	char buf[16];
	termo_strfkey_utf8 (g.tk, buf, sizeof buf, &fixed, TERMO_FORMAT_ALTISMETA);
	strv_append (out, buf);
}

static void
help_tab_assign_action (enum action action)
{
	hard_assert (g_help_tab.lines.len > g_help_tab.actions_len);

	size_t to_push = g_help_tab.lines.len - g_help_tab.actions_len;
	ARRAY_RESERVE (g_help_tab.actions, to_push);
	for (size_t i = 1; i < to_push; i++)
		g_help_tab.actions[g_help_tab.actions_len++] = ACTION_NONE;
	g_help_tab.actions[g_help_tab.actions_len++] = action;
}

static void
help_tab_group (struct binding *keys, size_t len, struct strv *out,
	bool bound[ACTION_COUNT])
{
	for (enum action i = 0; i < ACTION_COUNT; i++)
	{
		struct strv ass = strv_make ();
		for (size_t k = 0; k < len; k++)
			if (keys[k].action == i)
				help_tab_strfkey (&keys[k].decoded, &ass);
		if (ass.len)
		{
			char *joined = strv_join (&ass, ", ");
			strv_append_owned (out, xstrdup_printf
				("  %-30s %s", g_actions[i].description, joined));
			free (joined);

			bound[i] = true;
			help_tab_assign_action (i);
		}
		strv_free (&ass);
	}
}

static void
help_tab_unbound (struct strv *out, bool bound[ACTION_COUNT])
{
	for (enum action i = 0; i < ACTION_COUNT; i++)
		if (!bound[i])
		{
			strv_append_owned (out,
				xstrdup_printf ("  %-30s", g_actions[i].description));
			help_tab_assign_action (i);
		}
}

static void
help_tab_on_item_draw (size_t item_index, struct row_buffer *buffer, int width)
{
	(void) width;

	hard_assert (item_index < g_help_tab.lines.len);
	const char *line = g_help_tab.lines.vector[item_index];
	row_buffer_append (buffer, line, *line == ' ' ? 0 : A_BOLD);
}

static struct tab *
help_tab_init (void)
{
	ARRAY_INIT (g_help_tab.actions);
	struct strv *lines = &g_help_tab.lines;
	*lines = strv_make ();

	bool bound[ACTION_COUNT] = { [ACTION_NONE] = true };

	strv_append (lines, "Normal mode actions");
	help_tab_group (g_normal_keys, g_normal_keys_len, lines, bound);
	strv_append (lines, "");

	strv_append (lines, "Editor mode actions");
	help_tab_group (g_editor_keys, g_editor_keys_len, lines, bound);
	strv_append (lines, "");

	bool have_unbound = false;
	for (enum action i = 0; i < ACTION_COUNT; i++)
		if (!bound[i])
			have_unbound = true;

	if (have_unbound)
	{
		strv_append (lines, "Unbound actions");
		help_tab_unbound (lines, bound);
		strv_append (lines, "");
	}

	struct tab *super = &g_help_tab.super;
	tab_init (super, "Help");
	super->on_action = help_tab_on_action;
	super->on_item_draw = help_tab_on_item_draw;
	super->item_count = lines->len;
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

// --- Spectrum analyser -------------------------------------------------------

#ifdef WITH_FFTW

static void
spectrum_redraw (void)
{
	// A full refresh would be too computationally expensive,
	// let's hack around it in this case
	if (g.spectrum_row != -1)
	{
		// Don't mess up the line editor caret, when it's shown
		int last_x, last_y;
		getyx (stdscr, last_y, last_x);

		attrset (APP_ATTR (TAB_BAR));
		mvaddstr (g.spectrum_row, g.spectrum_column, g.spectrum.spectrum);
		attrset (0);

		move (last_y, last_x);
		refresh ();
	}
	else
		app_invalidate ();
}

// When any problem occurs with the FIFO, we'll just give up on it completely
static void
spectrum_discard_fifo (void)
{
	if (g.spectrum_fd != -1)
	{
		poller_fd_reset (&g.spectrum_event);
		xclose (g.spectrum_fd);
		g.spectrum_fd = -1;

		spectrum_free (&g.spectrum);
		g.spectrum_row = g.spectrum_column = -1;
		app_invalidate ();
	}
}

static void
spectrum_on_fifo_readable (const struct pollfd *pfd, void *user_data)
{
	(void) user_data;
	struct spectrum *s = &g.spectrum;

	bool update = false;
	ssize_t n;
restart:
	while ((n = read (pfd->fd,
		s->buffer + s->buffer_len, s->buffer_size - s->buffer_len)) > 0)
		if ((s->buffer_len += n) == s->buffer_size)
		{
			update = true;
			spectrum_sample (s);
			s->buffer_len = 0;
		}

	if (!n)
		spectrum_discard_fifo ();
	else if (errno == EINTR)
		goto restart;
	else if (errno != EAGAIN)
	{
		print_error ("spectrum: %s", strerror (errno));
		spectrum_discard_fifo ();
	}
	else if (update)
		spectrum_redraw ();
}

// When playback is stopped, we need to feed the analyser some zeroes ourselves.
// We could also just hide it.  Hard to say which is simpler or better.
static void
spectrum_clear (void)
{
	if (g.spectrum_fd != -1)
	{
		struct spectrum *s = &g.spectrum;
		memset (s->buffer, 0, s->buffer_size);
		spectrum_sample (s);
		spectrum_sample (s);
		s->buffer_len = 0;

		spectrum_redraw ();
	}
}

static void
spectrum_setup_fifo (void)
{
	const char *spectrum_path =
		get_config_string (g.config.root, "settings.spectrum_path");
	const char *spectrum_format =
		get_config_string (g.config.root, "settings.spectrum_format");
	struct config_item *spectrum_bars =
		config_item_get (g.config.root, "settings.spectrum_bars", NULL);
	if (!spectrum_path)
		return;

	struct error *e = NULL;
	char *path = resolve_filename
		(spectrum_path, resolve_relative_config_filename);

	if (!path)
		print_error ("spectrum: %s", "FIFO path could not be resolved");
	else if (!g.locale_is_utf8)
		print_error ("spectrum: %s", "UTF-8 locale required");
	else if (!spectrum_init (&g.spectrum,
		(char *) spectrum_format, spectrum_bars->value.integer, &e))
	{
		print_error ("spectrum: %s", e->message);
		error_free (e);
	}
	else if ((g.spectrum_fd = open (path, O_RDONLY | O_NONBLOCK)) == -1)
	{
		print_error ("spectrum: %s: %s", path, strerror (errno));
		spectrum_free (&g.spectrum);
	}
	else
	{
		g.spectrum_event = poller_fd_make (&g.poller, g.spectrum_fd);
		g.spectrum_event.dispatcher = spectrum_on_fifo_readable;
		poller_fd_set (&g.spectrum_event, POLLIN);
	}

	free (path);
}

#else  // ! WITH_FFTW
#define spectrum_setup_fifo()
#define spectrum_clear()
#define spectrum_discard_fifo()
#endif  // ! WITH_FFTW

// --- MPD interface -----------------------------------------------------------

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
mpd_set_elapsed_timer (int msec_past_second)
{
	int delay_msec = 1000 - msec_past_second;  // Until the next round second
	if (!g.elapsed_poll)
	{
		poller_timer_set (&g.elapsed_event, delay_msec);
		// Remember when the last round second was, relative to monotonic time
		g.elapsed_since = clock_msec (CLOCK_BEST) - msec_past_second;
		return;
	}

	// We may receive an earlier time, this seems to compensate for it well
	// (I haven't seen it trigger more than 50ms too early)
	delay_msec += 100;

	// When playback stalls, avoid busy looping with the server
	int elapsed_msec = g.song_elapsed * 1000 + msec_past_second;
	if (elapsed_msec == g.elapsed_since)
		delay_msec = MAX (delay_msec, 500);

	// In polling mode, we're interested in progress rather than stability.
	// We can reuse both the poller_timer struct and the timestamp field.
	poller_timer_set (&g.elapsed_event, delay_msec);
	g.elapsed_since = elapsed_msec;
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
	if (g.state == PLAYER_STOPPED)
	{
		spectrum_clear ();
	}

	// Values in "time" are always rounded.  "elapsed", introduced in MPD 0.16,
	// is in millisecond precision and "duration" as well, starting with 0.20.
	// Prefer the more precise values but use what we have.
	const char *time     = str_map_find (map, "time");
	const char *elapsed  = str_map_find (map, "elapsed");
	const char *duration = str_map_find (map, "duration");

	struct strv fields = strv_make ();
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

	poller_timer_reset (&g.elapsed_event);
	if (g.state == PLAYER_PLAYING)
		mpd_set_elapsed_timer (msec_past_second);
	else
		g.elapsed_since = -1;

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
mpd_process_info_data (const struct strv *data)
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
	struct str_map item = str_map_make (NULL);
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

/// Find a song by its id in the current playlist.  Expensive, rarely called.
static ssize_t
mpd_find_pos_of_id (const char *desired_id)
{
	compact_map_t map;
	const char *id;
	for (size_t i = 0; i < g.playlist.len; i++)
	{
		if ((map = item_list_get (&g.playlist, i))
		 && (id = compact_map_find (map, "id"))
		 && !strcmp (id, desired_id))
			return i;
	}
	return -1;
}

static char *
mpd_id_of_pos (int pos)
{
	compact_map_t map = item_list_get (&g.playlist, pos);
	return map ? compact_map_find (map,  "id") : NULL;
}

static void
mpd_process_info (const struct strv *data)
{
	int *selected = &g_current_tab.item_selected;
	int *marked   = &g_current_tab.item_mark;
	char *prev_sel_id  = mpd_id_of_pos (*selected);
	char *prev_mark_id = mpd_id_of_pos (*marked);
	if (prev_sel_id)   prev_sel_id  = xstrdup (prev_sel_id);
	if (prev_mark_id)  prev_mark_id = xstrdup (prev_mark_id);

	mpd_process_info_data (data);

	const char *sel_id  = mpd_id_of_pos (*selected);
	const char *mark_id = mpd_id_of_pos (*marked);

	if (prev_mark_id && (!mark_id || strcmp (prev_mark_id, mark_id)))
		*marked = mpd_find_pos_of_id (prev_mark_id);
	if (prev_sel_id  && (!sel_id  || strcmp (prev_sel_id,  sel_id)))
	{
		if ((*selected = mpd_find_pos_of_id (prev_sel_id)) < 0)
			*marked = -1;
		app_move_selection (0);
	}

	free (prev_sel_id);
	free (prev_mark_id);
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
mpd_on_elapsed_time_tick (void *user_data)
{
	(void) user_data;

	// Compute how much time has elapsed since the last round second
	int64_t diff_msec = clock_msec (CLOCK_BEST) - g.elapsed_since;
	int elapsed_sec = diff_msec / 1000;
	int elapsed_msec = diff_msec % 1000;

	g.song_elapsed += elapsed_sec;
	g.elapsed_since += elapsed_sec * 1000;

	// Try to get called on the next round second of playback
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
mpd_on_elapsed_time_tick_poll (void *user_data)
{
	(void) user_data;

	// As soon as the reply arrives, we (may) set the timer again
	mpd_request_info ();
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

// On an error, MPD discards the rest of our enqueuing commands--work it around
static void mpd_enqueue_step (size_t start_offset);

static void
mpd_on_enqueue_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) data;
	intptr_t start_offset = (intptr_t) user_data;

	if (response->success)
		strv_reset (&g.enqueue);
	else
	{
		// Their addition may also overflow, but YOLO
		hard_assert (start_offset >= 0 && response->list_offset >= 0);

		print_error ("%s: %s", response->message_text,
			g.enqueue.vector[start_offset + response->list_offset]);
		mpd_enqueue_step (start_offset + response->list_offset + 1);
	}
}

static void
mpd_enqueue_step (size_t start_offset)
{
	struct mpd_client *c = &g.client;
	if (start_offset >= g.enqueue.len)
	{
		strv_reset (&g.enqueue);
		return;
	}

	// TODO: might want to consider using addid and autoplaying
	mpd_client_list_begin (c);
	for (size_t i = start_offset; i < g.enqueue.len; i++)
		mpd_client_send_command (c, "add", g.enqueue.vector[i], NULL);
	mpd_client_list_end (c);
	mpd_client_add_task (c, mpd_on_enqueue_response, (void *) start_offset);
	mpd_client_idle (c, 0);
}

static void
mpd_on_ready (void)
{
	mpd_request_info ();
	library_tab_reload (NULL);
	spectrum_setup_fifo ();
	mpd_enqueue_step (0);
}

static void
mpd_on_password_response (const struct mpd_response *response,
	const struct strv *data, void *user_data)
{
	(void) data;
	(void) user_data;
	struct mpd_client *c = &g.client;

	if (response->success)
		mpd_on_ready ();
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
		mpd_on_ready ();
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

	spectrum_discard_fifo ();
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
			beep ();
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

	cstr_set (&g.message, NULL);
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

	struct str message = str_make ();
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
		cstr_set (&g.message, xstrdup (message.str));
		app_invalidate ();
		poller_timer_set (&g.message_timer, 5000);
	}
	str_free (&message);

	in_processing = false;
}

static void
app_init_poller_events (void)
{
	g.signal_event = poller_fd_make (&g.poller, g_signal_pipe[0]);
	g.signal_event.dispatcher = app_on_signal_pipe_readable;
	poller_fd_set (&g.signal_event, POLLIN);

	g.tty_event = poller_fd_make (&g.poller, STDIN_FILENO);
	g.tty_event.dispatcher = app_on_tty_readable;
	poller_fd_set (&g.tty_event, POLLIN);

	g.message_timer = poller_timer_make (&g.poller);
	g.message_timer.dispatcher = app_on_message_timer;

	g.tk_timer = poller_timer_make (&g.poller);
	g.tk_timer.dispatcher = app_on_key_timer;

	g.connect_event = poller_timer_make (&g.poller);
	g.connect_event.dispatcher = app_on_reconnect;
	poller_timer_set (&g.connect_event, 0);

	g.elapsed_event = poller_timer_make (&g.poller);
	g.elapsed_event.dispatcher = g.elapsed_poll
		? mpd_on_elapsed_time_tick_poll
		: mpd_on_elapsed_time_tick;

	g.refresh_event = poller_idle_make (&g.poller);
	g.refresh_event.dispatcher = app_on_refresh;
}

static void
app_init_enqueue (char *argv[], int argc)
{
	// TODO: MPD is unwilling to play directories, so perhaps recurse ourselves
	char cwd[4096] = "";
	for (int i = 0; i < argc; i++)
	{
		// This is a super-trivial method of URL detection, however anything
		// contaning the scheme and authority delimiters in a sequence is most
		// certainly not a filesystem path, and thus it will work as expected.
		// Error handling may be done by MPD.
		const char *path_or_URL = argv[i];
		if (*path_or_URL == '/' || strstr (path_or_URL, "://"))
			strv_append (&g.enqueue, path_or_URL);
		else if (!*cwd && !getcwd (cwd, sizeof cwd))
			exit_fatal ("getcwd: %s", strerror (errno));
		else
			strv_append_owned (&g.enqueue,
				xstrdup_printf ("%s/%s", cwd, path_or_URL));
	}
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

	struct opt_handler oh =
		opt_handler_make (argc, argv, opts,
			"[URL | PATH]...", "Terminal-based MPD client.");

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
	opt_handler_free (&oh);

	// We only need to convert to and from the terminal encoding
	if (!setlocale (LC_CTYPE, ""))
		print_warning ("failed to set the locale");

	app_init_context ();
	app_init_enqueue (argv, argc);
	app_load_configuration ();
	signals_setup_handlers ();
	app_init_poller_events ();
	app_init_terminal ();

	g_normal_keys = app_init_bindings ("normal",
		g_normal_defaults, N_ELEMENTS (g_normal_defaults), &g_normal_keys_len);
	g_editor_keys = app_init_bindings ("editor",
		g_editor_defaults, N_ELEMENTS (g_editor_defaults), &g_editor_keys_len);

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

	// TODO: the help tab should be the default for new users only,
	//   so provide a configuration option to flip this
	if (argc)
		app_switch_tab (&g_current_tab);

	g.polling = true;
	while (g.polling)
		poller_run (&g.poller);

	endwin ();
	g_log_message_real = log_message_stdio;
	app_free_context ();
	return 0;
}
