/*
 * line-editor.c: a line editor component for the TUI part of liberty
 *
 * Copyright (c) 2017 - 2022, Přemysl Eric Janouch <p@janouch.name>
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

// This is here just for IDE code model reasons
#ifndef HAVE_LIBERTY
#include "liberty/liberty.c"
#include "liberty/liberty-tui.c"
#endif

static void
row_buffer_append_c (struct row_buffer *self, ucs4_t c, chtype attrs)
{
	struct row_char current = { .attrs = attrs, .c = c };
	struct row_char invalid = { .attrs = attrs, .c = '?', .width = 1 };

	current.width = uc_width (current.c, locale_charset ());
	if (current.width < 0 || !app_is_character_in_locale (current.c))
		current = invalid;

	ARRAY_RESERVE (self->chars, 1);
	self->chars[self->chars_len++] = current;
	self->total_width += current.width;
}

// --- Line editor -------------------------------------------------------------

enum line_editor_action
{
	LINE_EDITOR_B_CHAR,                 ///< Go back a character
	LINE_EDITOR_F_CHAR,                 ///< Go forward a character
	LINE_EDITOR_B_WORD,                 ///< Go back a word
	LINE_EDITOR_F_WORD,                 ///< Go forward a word
	LINE_EDITOR_HOME,                   ///< Go to start of line
	LINE_EDITOR_END,                    ///< Go to end of line

	LINE_EDITOR_UPCASE_WORD,            ///< Convert word to uppercase
	LINE_EDITOR_DOWNCASE_WORD,          ///< Convert word to lowercase
	LINE_EDITOR_CAPITALIZE_WORD,        ///< Capitalize word

	LINE_EDITOR_B_DELETE,               ///< Delete last character
	LINE_EDITOR_F_DELETE,               ///< Delete next character
	LINE_EDITOR_B_KILL_WORD,            ///< Delete last word
	LINE_EDITOR_B_KILL_LINE,            ///< Delete everything up to BOL
	LINE_EDITOR_F_KILL_LINE,            ///< Delete everything up to EOL
};

struct line_editor
{
	int point;                          ///< Caret index into line data
	ucs4_t *line;                       ///< Line data, 0-terminated
	int *w;                             ///< Codepoint widths, 0-terminated
	size_t len;                         ///< Editor length
	size_t alloc;                       ///< Editor allocated
	char prompt;                        ///< Prompt character

	void (*on_changed) (void);          ///< Callback on text change
	void (*on_end) (bool);              ///< Callback on abort
};

static void
line_editor_free (struct line_editor *self)
{
	free (self->line);
	free (self->w);
}

/// Notify whomever invoked the editor that it's been either confirmed or
/// cancelled and clean up editor state
static void
line_editor_abort (struct line_editor *self, bool status)
{
	self->on_end (status);
	self->on_changed = NULL;

	free (self->line);
	self->line = NULL;
	free (self->w);
	self->w = NULL;
	self->alloc = 0;
	self->len = 0;
	self->point = 0;
	self->prompt = 0;
}

/// Start the line editor; remember to fill in "change" and "end" callbacks
static void
line_editor_start (struct line_editor *self, char prompt)
{
	self->alloc = 16;
	self->line = xcalloc (sizeof *self->line, self->alloc);
	self->w = xcalloc (sizeof *self->w, self->alloc);
	self->len = 0;
	self->point = 0;
	self->prompt = prompt;
}

static void
line_editor_changed (struct line_editor *self)
{
	self->line[self->len] = 0;
	self->w[self->len] = 0;

	if (self->on_changed)
		self->on_changed ();
}

static void
line_editor_move (struct line_editor *self, int to, int from, int len)
{
	memmove (self->line + to, self->line + from,
		sizeof *self->line * len);
	memmove (self->w + to, self->w + from,
		sizeof *self->w * len);
}

static void
line_editor_insert (struct line_editor *self, ucs4_t codepoint)
{
	while (self->alloc - self->len < 2 /* inserted + sentinel */)
	{
		self->alloc <<= 1;
		self->line = xreallocarray
			(self->line, sizeof *self->line, self->alloc);
		self->w = xreallocarray
			(self->w, sizeof *self->w, self->alloc);
	}

	line_editor_move (self, self->point + 1, self->point,
		self->len - self->point);
	self->line[self->point] = codepoint;
	self->w[self->point] = app_is_character_in_locale (codepoint)
		? uc_width (codepoint, locale_charset ())
		: 1 /* the replacement question mark */;

	self->point++;
	self->len++;
	line_editor_changed (self);
}

static bool
line_editor_action (struct line_editor *self, enum line_editor_action action)
{
	switch (action)
	{
	default:
		return soft_assert (!"unknown line editor action");

	case LINE_EDITOR_B_CHAR:
		if (self->point < 1)
			return false;
		do self->point--;
		while (self->point > 0
			&& !self->w[self->point]);
		return true;
	case LINE_EDITOR_F_CHAR:
		if (self->point + 1 > (int) self->len)
			return false;
		do self->point++;
		while (self->point < (int) self->len
			&& !self->w[self->point]);
		return true;
	case LINE_EDITOR_B_WORD:
	{
		if (self->point < 1)
			return false;
		int i = self->point;
		while (i && self->line[--i] == ' ');
		while (i-- && self->line[i] != ' ');
		self->point = ++i;
		return true;
	}
	case LINE_EDITOR_F_WORD:
	{
		if (self->point + 1 > (int) self->len)
			return false;
		int i = self->point;
		while (i < (int) self->len && self->line[i] == ' ') i++;
		while (i < (int) self->len && self->line[i] != ' ') i++;
		self->point = i;
		return true;
	}
	case LINE_EDITOR_HOME:
		self->point = 0;
		return true;
	case LINE_EDITOR_END:
		self->point = self->len;
		return true;

	case LINE_EDITOR_UPCASE_WORD:
	{
		int i = self->point;
		for (; i < (int) self->len && self->line[i] == ' '; i++);
		for (; i < (int) self->len && self->line[i] != ' '; i++)
			self->line[i] = uc_toupper (self->line[i]);
		self->point = i;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_DOWNCASE_WORD:
	{
		int i = self->point;
		for (; i < (int) self->len && self->line[i] == ' '; i++);
		for (; i < (int) self->len && self->line[i] != ' '; i++)
			self->line[i] = uc_tolower (self->line[i]);
		self->point = i;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_CAPITALIZE_WORD:
	{
		int i = self->point;
		ucs4_t (*converter) (ucs4_t) = uc_totitle;
		for (; i < (int) self->len && self->line[i] == ' '; i++);
		for (; i < (int) self->len && self->line[i] != ' '; i++)
		{
			self->line[i] = converter (self->line[i]);
			converter = uc_tolower;
		}
		self->point = i;
		line_editor_changed (self);
		return true;
	}

	case LINE_EDITOR_B_DELETE:
	{
		if (self->point < 1)
			return false;
		int len = 1;
		while (self->point - len > 0
			&& !self->w[self->point - len])
			len++;
		line_editor_move (self, self->point - len, self->point,
			self->len - self->point);
		self->len -= len;
		self->point -= len;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_F_DELETE:
	{
		if (self->point + 1 > (int) self->len)
			return false;
		int len = 1;
		while (self->point + len < (int) self->len
			&& !self->w[self->point + len])
			len++;
		self->len -= len;
		line_editor_move (self, self->point, self->point + len,
			self->len - self->point);
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_B_KILL_WORD:
	{
		if (self->point < 1)
			return false;

		int i = self->point;
		while (i && self->line[--i] == ' ');
		while (i-- && self->line[i] != ' ');
		i++;

		line_editor_move (self, i, self->point, (self->len - self->point));
		self->len -= self->point - i;
		self->point = i;
		line_editor_changed (self);
		return true;
	}
	case LINE_EDITOR_B_KILL_LINE:
		self->len -= self->point;
		line_editor_move (self, 0, self->point, self->len);
		self->point = 0;
		line_editor_changed (self);
		return true;
	case LINE_EDITOR_F_KILL_LINE:
		self->len = self->point;
		line_editor_changed (self);
		return true;
	}
}

static int
line_editor_write (const struct line_editor *self, struct row_buffer *row,
	int width, chtype attrs)
{
	if (self->prompt)
	{
		hard_assert (self->prompt < 127);
		row_buffer_append_c (row, self->prompt, attrs);
		width--;
	}

	int following = 0;
	for (size_t i = self->point; i < self->len; i++)
		following += self->w[i];

	int preceding = 0;
	size_t start = self->point;
	while (start && preceding < width / 2)
		preceding += self->w[--start];

	// There can be one extra space at the end of the line but this way we
	// don't need to care about non-spacing marks following full-width chars
	while (start && width - preceding - following > 2 /* widest char */)
		preceding += self->w[--start];

	// XXX: we should also show < > indicators for overflow but it'd probably
	//   considerably complicate this algorithm
	for (; start < self->len; start++)
		row_buffer_append_c (row, self->line[start], attrs);
	return !!self->prompt + preceding;
}
