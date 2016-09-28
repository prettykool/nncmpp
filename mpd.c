// Copied from desktop-tools, should go to liberty if it proves useful

// --- MPD client interface ----------------------------------------------------

// This is a rather thin MPD client interface intended for basic tasks

#define MPD_SUBSYSTEM_TABLE(XX)                 \
	XX (DATABASE,         0, "database")        \
	XX (UPDATE,           1, "update")          \
	XX (STORED_PLAYLIST,  2, "stored_playlist") \
	XX (PLAYLIST,         3, "playlist")        \
	XX (PLAYER,           4, "player")          \
	XX (MIXER,            5, "mixer")           \
	XX (OUTPUT,           6, "output")          \
	XX (OPTIONS,          7, "options")         \
	XX (STICKER,          8, "sticker")         \
	XX (SUBSCRIPTION,     9, "subscription")    \
	XX (MESSAGE,         10, "message")

enum mpd_subsystem
{
#define XX(a, b, c) MPD_SUBSYSTEM_ ## a = (1 << b),
	MPD_SUBSYSTEM_TABLE (XX)
#undef XX
	MPD_SUBSYSTEM_MAX
};

static const char *mpd_subsystem_names[] =
{
#define XX(a, b, c) [b] = c,
	MPD_SUBSYSTEM_TABLE (XX)
#undef XX
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

enum mpd_client_state
{
	MPD_DISCONNECTED,                   ///< Not connected
	MPD_CONNECTING,                     ///< Currently connecting
	MPD_CONNECTED                       ///< Connected
};

struct mpd_response
{
	bool success;                       ///< OK or ACK

	// ACK-only fields:

	int error;                          ///< Numeric error value (ack.h)
	int list_offset;                    ///< Offset of command in list
	char *current_command;              ///< Name of the erroring command
	char *message_text;                 ///< Error message
};

/// Task completion callback
typedef void (*mpd_client_task_cb) (const struct mpd_response *response,
	const struct str_vector *data, void *user_data);

struct mpd_client_task
{
	LIST_HEADER (struct mpd_client_task)

	mpd_client_task_cb callback;        ///< Callback on completion
	void *user_data;                    ///< User data
};

struct mpd_client
{
	struct poller *poller;              ///< Poller

	// Connection:

	enum mpd_client_state state;        ///< Connection state
	struct connector *connector;        ///< Connection establisher

	int socket;                         ///< MPD socket
	struct str read_buffer;             ///< Input yet to be processed
	struct str write_buffer;            ///< Outut yet to be be sent out
	struct poller_fd socket_event;      ///< We can read from the socket

	struct poller_timer timeout_timer;  ///< Connection seems to be dead

	// Protocol:

	bool got_hello;                     ///< Got the OK MPD hello message

	bool idling;                        ///< Sent idle as the last command
	unsigned idling_subsystems;         ///< Subsystems we're idling for
	bool in_list;                       ///< We're inside a command list

	struct mpd_client_task *tasks;      ///< Task queue
	struct mpd_client_task *tasks_tail; ///< Tail of task queue
	struct str_vector data;             ///< Data from last command

	// User configuration:

	void *user_data;                    ///< User data for callbacks

	/// Callback after connection has been successfully established
	void (*on_connected) (void *user_data);

	/// Callback for general failures or even normal disconnection;
	/// the interface is reinitialized
	void (*on_failure) (void *user_data);

	/// Callback to receive "idle" updates.
	/// Remember to restart the idle if needed.
	void (*on_event) (unsigned subsystems, void *user_data);
};

static void mpd_client_reset (struct mpd_client *self);
static void mpd_client_destroy_connector (struct mpd_client *self);

static void
mpd_client_init (struct mpd_client *self, struct poller *poller)
{
	memset (self, 0, sizeof *self);

	self->poller = poller;
	self->socket = -1;

	str_init (&self->read_buffer);
	str_init (&self->write_buffer);

	str_vector_init (&self->data);

	poller_fd_init (&self->socket_event, poller, -1);
	poller_timer_init (&self->timeout_timer, poller);
}

static void
mpd_client_free (struct mpd_client *self)
{
	// So that we don't have to repeat most of the stuff
	mpd_client_reset (self);

	str_free (&self->read_buffer);
	str_free (&self->write_buffer);

	str_vector_free (&self->data);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Reinitialize the interface so that you can reconnect anew
static void
mpd_client_reset (struct mpd_client *self)
{
	if (self->state == MPD_CONNECTING)
		mpd_client_destroy_connector (self);

	if (self->socket != -1)
		xclose (self->socket);
	self->socket = -1;

	self->socket_event.closed = true;
	poller_fd_reset (&self->socket_event);
	poller_timer_reset (&self->timeout_timer);

	str_reset (&self->read_buffer);
	str_reset (&self->write_buffer);

	str_vector_reset (&self->data);

	self->got_hello = false;
	self->idling = false;
	self->idling_subsystems = 0;
	self->in_list = false;

	LIST_FOR_EACH (struct mpd_client_task, iter, self->tasks)
		free (iter);
	self->tasks = self->tasks_tail = NULL;

	self->state = MPD_DISCONNECTED;
}

static void
mpd_client_fail (struct mpd_client *self)
{
	mpd_client_reset (self);
	if (self->on_failure)
		self->on_failure (self->user_data);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_client_parse_response (const char *p, struct mpd_response *response)
{
	if (!strcmp (p, "OK"))
		return response->success = true;
	if (!strcmp (p, "list_OK"))
		// TODO: either implement this or fail the connection properly
		hard_assert (!"command_list_ok_begin not implemented");

	char *end = NULL;
	if (*p++ != 'A' || *p++ != 'C' || *p++ != 'K' || *p++ != ' ' || *p++ != '[')
		return false;

	errno = 0;
	response->error = strtoul (p, &end, 10);
	if (errno != 0 || end == p)
		return false;
	p = end;
	if (*p++ != '@')
		return false;

	errno = 0;
	response->list_offset = strtoul (p, &end, 10);
	if (errno != 0 || end == p)
		return false;
	p = end;
	if (*p++ != ']' || *p++ != ' ' || *p++ != '{' || !(end = strchr (p, '}')))
		return false;

	response->current_command = xstrndup (p, end - p);
	p = end + 1;

	if (*p++ != ' ')
		return false;

	response->message_text = xstrdup (p);
	response->success = false;
	return true;
}

static void
mpd_client_dispatch (struct mpd_client *self, struct mpd_response *response)
{
	struct mpd_client_task *task;
	if (!(task = self->tasks))
		return;

	if (task->callback)
		task->callback (response, &self->data, task->user_data);
	str_vector_reset (&self->data);

	LIST_UNLINK_WITH_TAIL (self->tasks, self->tasks_tail, task);
	free (task);
}

static bool
mpd_client_parse_hello (struct mpd_client *self, const char *line)
{
	const char hello[] = "OK MPD ";
	if (strncmp (line, hello, sizeof hello - 1))
	{
		print_debug ("invalid MPD hello message");
		return false;
	}

	// TODO: call "on_connected" now.  We should however also set up a timer
	//   so that we don't wait on this message forever.
	return self->got_hello = true;
}

static bool
mpd_client_parse_line (struct mpd_client *self, const char *line)
{
	print_debug ("MPD >> %s", line);

	if (!self->got_hello)
		return mpd_client_parse_hello (self, line);

	struct mpd_response response;
	memset (&response, 0, sizeof response);
	if (mpd_client_parse_response (line, &response))
	{
		mpd_client_dispatch (self, &response);
		free (response.current_command);
		free (response.message_text);
	}
	else
		str_vector_add (&self->data, line);
	return true;
}

/// All output from MPD commands seems to be in a trivial "key: value" format
static char *
mpd_client_parse_kv (char *line, char **value)
{
	char *sep;
	if (!(sep = strstr (line, ": ")))
		return NULL;

	*sep = 0;
	*value = sep + 2;
	return line;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_update_poller (struct mpd_client *self)
{
	poller_fd_set (&self->socket_event,
		self->write_buffer.len ? (POLLIN | POLLOUT) : POLLIN);
}

static bool
mpd_client_process_input (struct mpd_client *self)
{
	// Split socket input at newlines and process them separately
	struct str *rb = &self->read_buffer;
	char *start = rb->str, *end = start + rb->len;
	for (char *p = start; p < end; p++)
	{
		if (*p != '\n')
			continue;

		*p = 0;
		if (!mpd_client_parse_line (self, start))
			return false;
		start = p + 1;
	}

	str_remove_slice (rb, 0, start - rb->str);
	return true;
}

static void
mpd_client_on_ready (const struct pollfd *pfd, void *user_data)
{
	(void) pfd;

	struct mpd_client *self = user_data;
	if (socket_io_try_read  (self->socket, &self->read_buffer)  != SOCKET_IO_OK
	 || !mpd_client_process_input (self)
	 || socket_io_try_write (self->socket, &self->write_buffer) != SOCKET_IO_OK)
		mpd_client_fail (self);
	else
		mpd_client_update_poller (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_client_must_quote_char (char c)
{
	return (unsigned char) c <= ' ' || c == '"' || c == '\'';
}

static bool
mpd_client_must_quote (const char *s)
{
	if (!*s)
		return true;
	for (; *s; s++)
		if (mpd_client_must_quote_char (*s))
			return true;
	return false;
}

static void
mpd_client_quote (const char *s, struct str *output)
{
	str_append_c (output, '"');
	for (; *s; s++)
	{
		if (mpd_client_must_quote_char (*s))
			str_append_c (output, '\\');
		str_append_c (output, *s);
	}
	str_append_c (output, '"');
}

/// Beware that delivery of the event isn't deferred and you musn't make
/// changes to the interface while processing the event!
static void
mpd_client_add_task
	(struct mpd_client *self, mpd_client_task_cb cb, void *user_data)
{
	// This only has meaning with command_list_ok_begin, and then it requires
	// special handling (all in-list tasks need to be specially marked and
	// later flushed if an early ACK or OK arrives).
	hard_assert (!self->in_list);

	struct mpd_client_task *task = xcalloc (1, sizeof *self);
	task->callback = cb;
	task->user_data = user_data;
	LIST_APPEND_WITH_TAIL (self->tasks, self->tasks_tail, task);
}

/// Send a command.  Remember to call mpd_client_add_task() to handle responses,
/// unless the command is being sent in a list.
static void mpd_client_send_command
	(struct mpd_client *self, const char *command, ...) ATTRIBUTE_SENTINEL;

static void
mpd_client_send_commandv (struct mpd_client *self, char **commands)
{
	// Automatically interrupt idle mode
	if (self->idling)
	{
		poller_timer_reset (&self->timeout_timer);

		self->idling = false;
		self->idling_subsystems = 0;
		mpd_client_send_command (self, "noidle", NULL);
	}

	struct str line;
	str_init (&line);

	for (; *commands; commands++)
	{
		if (line.len)
			str_append_c (&line, ' ');

		if (mpd_client_must_quote (*commands))
			mpd_client_quote (*commands, &line);
		else
			str_append (&line, *commands);
	}

	print_debug ("MPD << %s", line.str);
	str_append_c (&line, '\n');
	str_append_str (&self->write_buffer, &line);
	str_free (&line);

	mpd_client_update_poller (self);
}

static void
mpd_client_send_command (struct mpd_client *self, const char *command, ...)
{
	struct str_vector v;
	str_vector_init (&v);

	va_list ap;
	va_start (ap, command);
	for (; command; command = va_arg (ap, const char *))
		str_vector_add (&v, command);
	va_end (ap);

	mpd_client_send_commandv (self, v.vector);
	str_vector_free (&v);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_list_begin (struct mpd_client *self)
{
	hard_assert (!self->in_list);
	mpd_client_send_command (self, "command_list_begin", NULL);
	self->in_list = true;
}

/// End a list of commands.  Remember to call mpd_client_add_task()
/// to handle the summary response.
static void
mpd_client_list_end (struct mpd_client *self)
{
	hard_assert (self->in_list);
	mpd_client_send_command (self, "command_list_end", NULL);
	self->in_list = false;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static bool
mpd_resolve_subsystem (const char *name, unsigned *output)
{
	for (size_t i = 0; i < N_ELEMENTS (mpd_subsystem_names); i++)
		if (!strcasecmp_ascii (name, mpd_subsystem_names[i]))
		{
			*output |= 1 << i;
			return true;
		}
	return false;
}

static void
mpd_client_on_idle_return (const struct mpd_response *response,
	const struct str_vector *data, void *user_data)
{
	(void) response;

	struct mpd_client *self = user_data;
	unsigned subsystems = 0;
	for (size_t i = 0; i < data->len; i++)
	{
		char *value, *key;
		if (!(key = mpd_client_parse_kv (data->vector[i], &value)))
			print_debug ("%s: %s", "erroneous MPD output", data->vector[i]);
		else if (strcasecmp_ascii (key, "changed"))
			print_debug ("%s: %s", "unexpected idle key", key);
		else if (!mpd_resolve_subsystem (value, &subsystems))
			print_debug ("%s: %s", "unknown subsystem", value);
	}

	// Not resetting "idling" here, we may send an extra "noidle" no problem
	if (self->on_event && subsystems)
		self->on_event (subsystems, self->user_data);
}

static void mpd_client_idle (struct mpd_client *self, unsigned subsystems);

static void
mpd_client_on_timeout (void *user_data)
{
	struct mpd_client *self = user_data;
	unsigned subsystems = self->idling_subsystems;

	// Just sending this out should bring a dead connection down over TCP
	// TODO: set another timer to make sure the ping reply arrives
	mpd_client_send_command (self, "ping", NULL);
	mpd_client_add_task (self, NULL, NULL);

	// Restore the incriminating idle immediately
	mpd_client_idle (self, subsystems);
}

/// When not expecting to send any further commands, you should call this
/// in order to keep the connection alive.  Or to receive updates.
static void
mpd_client_idle (struct mpd_client *self, unsigned subsystems)
{
	hard_assert (!self->in_list);

	struct str_vector v;
	str_vector_init (&v);

	str_vector_add (&v, "idle");
	for (size_t i = 0; i < N_ELEMENTS (mpd_subsystem_names); i++)
		if (subsystems & (1 << i))
			str_vector_add (&v, mpd_subsystem_names[i]);

	mpd_client_send_commandv (self, v.vector);
	str_vector_free (&v);

	self->timeout_timer.dispatcher = mpd_client_on_timeout;
	self->timeout_timer.user_data = self;
	poller_timer_set (&self->timeout_timer, 5 * 60 * 1000);

	mpd_client_add_task (self, mpd_client_on_idle_return, self);
	self->idling = true;
	self->idling_subsystems = subsystems;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
mpd_client_finish_connection (struct mpd_client *self, int socket)
{
	set_blocking (socket, false);
	self->socket = socket;
	self->state = MPD_CONNECTED;

	poller_fd_init (&self->socket_event, self->poller, self->socket);
	self->socket_event.dispatcher = mpd_client_on_ready;
	self->socket_event.user_data = self;

	mpd_client_update_poller (self);

	if (self->on_connected)
		self->on_connected (self->user_data);
}

static void
mpd_client_destroy_connector (struct mpd_client *self)
{
	if (self->connector)
		connector_free (self->connector);
	free (self->connector);
	self->connector = NULL;

	// Not connecting anymore
	self->state = MPD_DISCONNECTED;
}

static void
mpd_client_on_connector_failure (void *user_data)
{
	struct mpd_client *self = user_data;
	mpd_client_destroy_connector (self);
	mpd_client_fail (self);
}

static void
mpd_client_on_connector_connected
	(void *user_data, int socket, const char *host)
{
	(void) host;

	struct mpd_client *self = user_data;
	mpd_client_destroy_connector (self);
	mpd_client_finish_connection (self, socket);
}

static bool
mpd_client_connect_unix (struct mpd_client *self, const char *address,
	struct error **e)
{
	int fd = socket (AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
	{
		error_set (e, "%s: %s", "socket", strerror (errno));
		return false;
	}

	// Expand tilde if needed
	char *expanded = resolve_filename (address, xstrdup);

	struct sockaddr_un sun;
	sun.sun_family = AF_UNIX;
	strncpy (sun.sun_path, expanded, sizeof sun.sun_path);
	sun.sun_path[sizeof sun.sun_path - 1] = 0;

	free (expanded);

	if (connect (fd, (struct sockaddr *) &sun, sizeof sun))
	{
		error_set (e, "%s: %s", "connect", strerror (errno));
		return false;
	}

	mpd_client_finish_connection (self, fd);
	return true;
}

static bool
mpd_client_connect (struct mpd_client *self, const char *address,
	const char *service, struct error **e)
{
	hard_assert (self->state == MPD_DISCONNECTED);

	// If it looks like a path, assume it's a UNIX socket
	if (strchr (address, '/'))
		return mpd_client_connect_unix (self, address, e);

	struct connector *connector = xmalloc (sizeof *connector);
	connector_init (connector, self->poller);
	self->connector = connector;

	connector->user_data    = self;
	connector->on_connected = mpd_client_on_connector_connected;
	connector->on_failure   = mpd_client_on_connector_failure;

	connector_add_target (connector, address, service);
	self->state = MPD_CONNECTING;
	return true;
}
