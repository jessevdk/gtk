#include "gdkbroadway-server.h"

#include "broadway.h"
#include "gdkprivate-broadway.h"

#include <glib.h>
#include <glib/gprintf.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

typedef struct BroadwayInput BroadwayInput;

struct _GdkBroadwayServer {
  GObject parent_instance;

  int port;
  GSocketService *service;
  BroadwayOutput *output;
  guint32 id_counter;
  guint32 saved_serial;
  guint64 last_seen_time;
  BroadwayInput *input;
  GList *input_messages;
  guint process_input_idle;

  GHashTable *id_ht;
  GList *toplevels;

  gint32 mouse_in_toplevel_id;
  int last_x, last_y; /* in root coords */
  guint32 last_state;
  gint32 real_mouse_in_toplevel_id; /* Not affected by grabs */

  /* Explicit pointer grabs: */
  gint32 pointer_grab_window_id; /* -1 => none */
  guint32 pointer_grab_time;
  gboolean pointer_grab_owner_events;

  /* Future data, from the currently queued events */
  int future_root_x;
  int future_root_y;
  guint32 future_state;
  int future_mouse_in_toplevel;
};

struct _GdkBroadwayServerClass
{
  GObjectClass parent_class;
};

typedef struct HttpRequest {
  GdkBroadwayServer *server;
  GSocketConnection *connection;
  GDataInputStream *data;
  GString *request;
}  HttpRequest;

struct BroadwayInput {
  GdkBroadwayServer *server;
  GSocketConnection *connection;
  GByteArray *buffer;
  GSource *source;
  gboolean seen_time;
  gint64 time_base;
  gboolean proto_v7_plus;
  gboolean binary;
};

typedef struct {
  gint32 id;
  gint32 x;
  gint32 y;
  gint32 width;
  gint32 height;
  gboolean is_temp;
  gboolean last_synced;
  gboolean visible;
  gint32 transient_for;

  cairo_surface_t *last_surface;
} BroadwayWindow;

static void _gdk_broadway_server_resync_windows (GdkBroadwayServer *server);

G_DEFINE_TYPE (GdkBroadwayServer, gdk_broadway_server, G_TYPE_OBJECT)

static void
gdk_broadway_server_init (GdkBroadwayServer *server)
{
  BroadwayWindow *root;

  server->service = g_socket_service_new ();
  server->pointer_grab_window_id = -1;
  server->saved_serial = 1;
  server->last_seen_time = 1;
  server->id_ht = g_hash_table_new (NULL, NULL);
  server->id_counter = 0;

  root = g_new0 (BroadwayWindow, 1);
  root->id = server->id_counter++;
  root->width = 1024;
  root->height = 768;
  root->visible = TRUE;

  g_hash_table_insert (server->id_ht,
		       GINT_TO_POINTER (root->id),
		       root);
}

static void
gdk_broadway_server_finalize (GObject *object)
{
  G_OBJECT_CLASS (gdk_broadway_server_parent_class)->finalize (object);
}

static void
gdk_broadway_server_class_init (GdkBroadwayServerClass * class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = gdk_broadway_server_finalize;
}

static void start_output (HttpRequest *request, gboolean proto_v7_plus, gboolean binary);

static void
http_request_free (HttpRequest *request)
{
  g_object_unref (request->connection);
  g_object_unref (request->data);
  g_string_free (request->request, TRUE);
  g_free (request);
}

static void
broadway_input_free (BroadwayInput *input)
{
  g_object_unref (input->connection);
  g_byte_array_free (input->buffer, FALSE);
  g_source_destroy (input->source);
  g_free (input);
}

static void
update_event_state (GdkBroadwayServer *server,
		    BroadwayInputMsg *message)
{
  switch (message->base.type) {
  case BROADWAY_EVENT_ENTER:
    server->last_x = message->pointer.root_x;
    server->last_y = message->pointer.root_y;
    server->last_state = message->pointer.state;
    server->real_mouse_in_toplevel_id = message->pointer.mouse_window_id;

    /* TODO: Unset when it dies */
    server->mouse_in_toplevel_id = message->pointer.event_window_id;
    break;
  case BROADWAY_EVENT_LEAVE:
    server->last_x = message->pointer.root_x;
    server->last_y = message->pointer.root_y;
    server->last_state = message->pointer.state;
    server->real_mouse_in_toplevel_id = message->pointer.mouse_window_id;

    server->mouse_in_toplevel_id = 0;
    break;
  case BROADWAY_EVENT_POINTER_MOVE:
    server->last_x = message->pointer.root_x;
    server->last_y = message->pointer.root_y;
    server->last_state = message->pointer.state;
    server->real_mouse_in_toplevel_id = message->pointer.mouse_window_id;
    break;
  case BROADWAY_EVENT_BUTTON_PRESS:
  case BROADWAY_EVENT_BUTTON_RELEASE:
    server->last_x = message->pointer.root_x;
    server->last_y = message->pointer.root_y;
    server->last_state = message->pointer.state;
    server->real_mouse_in_toplevel_id = message->pointer.mouse_window_id;
    break;
  case BROADWAY_EVENT_SCROLL:
    server->last_x = message->pointer.root_x;
    server->last_y = message->pointer.root_y;
    server->last_state = message->pointer.state;
    server->real_mouse_in_toplevel_id = message->pointer.mouse_window_id;
    break;
  case BROADWAY_EVENT_KEY_PRESS:
  case BROADWAY_EVENT_KEY_RELEASE:
    server->last_state = message->key.state;
    break;
  case BROADWAY_EVENT_GRAB_NOTIFY:
  case BROADWAY_EVENT_UNGRAB_NOTIFY:
    break;
  case BROADWAY_EVENT_CONFIGURE_NOTIFY:
    break;
  case BROADWAY_EVENT_DELETE_NOTIFY:
    break;
  case BROADWAY_EVENT_SCREEN_SIZE_CHANGED:
    break;

  default:
    g_printerr ("update_event_state - Unknown input command %c\n", message->base.type);
    break;
  }
}

gboolean
_gdk_broadway_server_lookahead_event (GdkBroadwayServer  *server,
				      const char         *types)
{
  BroadwayInputMsg *message;
  GList *l;

  for (l = server->input_messages; l != NULL; l = l->next)
    {
      message = l->data;
      if (strchr (types, message->base.type) != NULL)
	return TRUE;
    }

  return FALSE;
}

static void
process_input_messages (GdkBroadwayServer *server)
{
  BroadwayInputMsg *message;

  while (server->input_messages)
    {
      message = server->input_messages->data;
      server->input_messages =
	g_list_delete_link (server->input_messages,
			    server->input_messages);


      update_event_state (server, message);
      _gdk_broadway_events_got_input (message);
      g_free (message);
    }
}

static char *
parse_pointer_data (char *p, BroadwayInputPointerMsg *data)
{
  data->mouse_window_id = strtol (p, &p, 10);
  p++; /* Skip , */
  data->event_window_id = strtol (p, &p, 10);
  p++; /* Skip , */
  data->root_x = strtol (p, &p, 10);
  p++; /* Skip , */
  data->root_y = strtol (p, &p, 10);
  p++; /* Skip , */
  data->win_x = strtol (p, &p, 10);
  p++; /* Skip , */
  data->win_y = strtol (p, &p, 10);
  p++; /* Skip , */
  data->state = strtol (p, &p, 10);

  return p;
}

static void
update_future_pointer_info (GdkBroadwayServer *server, BroadwayInputPointerMsg *data)
{
  server->future_root_x = data->root_x;
  server->future_root_y = data->root_y;
  server->future_state = data->state;
  server->future_mouse_in_toplevel = data->mouse_window_id;
}

static void
parse_input_message (BroadwayInput *input, const char *message)
{
  GdkBroadwayServer *server = input->server;
  BroadwayInputMsg msg;
  char *p;
  gint64 time_;

  p = (char *)message;
  msg.base.type = *p++;
  msg.base.serial = (guint32)strtol (p, &p, 10);
  p++; /* Skip , */
  time_ = strtol(p, &p, 10);
  p++; /* Skip , */

  if (time_ == 0) {
    time_ = server->last_seen_time;
  } else {
    if (!input->seen_time) {
      input->seen_time = TRUE;
      /* Calculate time base so that any following times are normalized to start
	 5 seconds after last_seen_time, to avoid issues that could appear when
	 a long hiatus due to a reconnect seems to be instant */
      input->time_base = time_ - (server->last_seen_time + 5000);
    }
    time_ = time_ - input->time_base;
  }

  server->last_seen_time = time_;

  msg.base.time = time_;

  switch (msg.base.type) {
  case BROADWAY_EVENT_ENTER:
  case BROADWAY_EVENT_LEAVE:
    p = parse_pointer_data (p, &msg.pointer);
    update_future_pointer_info (server, &msg.pointer);
    p++; /* Skip , */
    msg.crossing.mode = strtol(p, &p, 10);
    break;

  case BROADWAY_EVENT_POINTER_MOVE: /* Mouse move */
    p = parse_pointer_data (p, &msg.pointer);
    update_future_pointer_info (server, &msg.pointer);
    break;

  case BROADWAY_EVENT_BUTTON_PRESS:
  case BROADWAY_EVENT_BUTTON_RELEASE:
    p = parse_pointer_data (p, &msg.pointer);
    update_future_pointer_info (server, &msg.pointer);
    p++; /* Skip , */
    msg.button.button = strtol(p, &p, 10);
    break;

  case BROADWAY_EVENT_SCROLL:
    p = parse_pointer_data (p, &msg.pointer);
    update_future_pointer_info (server, &msg.pointer);
    p++; /* Skip , */
    msg.scroll.dir = strtol(p, &p, 10);
    break;

  case BROADWAY_EVENT_KEY_PRESS:
  case BROADWAY_EVENT_KEY_RELEASE:
    msg.key.mouse_window_id = strtol(p, &p, 10);
    p++; /* Skip , */
    msg.key.key = strtol(p, &p, 10);
    p++; /* Skip , */
    msg.key.state = strtol(p, &p, 10);
    break;

  case BROADWAY_EVENT_GRAB_NOTIFY:
  case BROADWAY_EVENT_UNGRAB_NOTIFY:
    msg.grab_reply.res = strtol(p, &p, 10);
    break;

  case BROADWAY_EVENT_CONFIGURE_NOTIFY:
    msg.configure_notify.id = strtol(p, &p, 10);
    p++; /* Skip , */
    msg.configure_notify.x = strtol (p, &p, 10);
    p++; /* Skip , */
    msg.configure_notify.y = strtol (p, &p, 10);
    p++; /* Skip , */
    msg.configure_notify.width = strtol (p, &p, 10);
    p++; /* Skip , */
    msg.configure_notify.height = strtol (p, &p, 10);
    break;

  case BROADWAY_EVENT_DELETE_NOTIFY:
    msg.delete_notify.id = strtol(p, &p, 10);
    break;

  case BROADWAY_EVENT_SCREEN_SIZE_CHANGED:
    msg.screen_resize_notify.width = strtol (p, &p, 10);
    p++; /* Skip , */
    msg.screen_resize_notify.height = strtol (p, &p, 10);
    break;

  default:
    g_printerr ("parse_input_message - Unknown input command %c (%s)\n", msg.base.type, message);
    break;
  }

  server->input_messages = g_list_append (server->input_messages, g_memdup (&msg, sizeof (msg)));

}

static inline void
hex_dump (guchar *data, gsize len)
{
#ifdef DEBUG_WEBSOCKETS
  gsize i, j;
  for (j = 0; j < len + 15; j += 16)
    {
      fprintf (stderr, "0x%.4x  ", j);
      for (i = 0; i < 16; i++)
	{
	    if ((j + i) < len)
	      fprintf (stderr, "%.2x ", data[j+i]);
	    else
	      fprintf (stderr, "  ");
	    if (i == 8)
	      fprintf (stderr, " ");
	}
      fprintf (stderr, " | ");

      for (i = 0; i < 16; i++)
	if ((j + i) < len && g_ascii_isalnum(data[j+i]))
	  fprintf (stderr, "%c", data[j+i]);
	else
	  fprintf (stderr, ".");
      fprintf (stderr, "\n");
    }
#endif
}

static void
parse_input (BroadwayInput *input)
{
  GdkBroadwayServer *server = input->server;

  if (!input->buffer->len)
    return;

  if (input->proto_v7_plus)
    {
      hex_dump (input->buffer->data, input->buffer->len);

      while (input->buffer->len > 2)
	{
	  gsize len, payload_len;
	  BroadwayWSOpCode code;
	  gboolean is_mask, fin;
	  guchar *buf, *data, *mask;

	  buf = input->buffer->data;
	  len = input->buffer->len;

#ifdef DEBUG_WEBSOCKETS
	  g_print ("Parse input first byte 0x%2x 0x%2x\n", buf[0], buf[1]);
#endif

	  fin = buf[0] & 0x80;
	  code = buf[0] & 0x0f;
	  payload_len = buf[1] & 0x7f;
	  is_mask = buf[1] & 0x80;
	  data = buf + 2;

	  if (payload_len > 125)
	    {
	      if (len < 4)
		return;
	      payload_len = GUINT16_FROM_BE( *(guint16 *) data );
	      data += 2;
	    }
	  else if (payload_len > 126)
	    {
	      if (len < 10)
		return;
	      payload_len = GUINT64_FROM_BE( *(guint64 *) data );
	      data += 8;
	    }

	  mask = NULL;
	  if (is_mask)
	    {
	      if (data - buf + 4 > len)
		return;
	      mask = data;
	      data += 4;
	    }

	  if (data - buf + payload_len > len)
	    return; /* wait to accumulate more */

	  if (is_mask)
	    {
	      gsize i;
	      for (i = 0; i < payload_len; i++)
		data[i] ^= mask[i%4];
	    }

	  switch (code) {
	  case BROADWAY_WS_CNX_CLOSE:
	    break; /* hang around anyway */
	  case BROADWAY_WS_TEXT:
	    if (!fin)
	      {
#ifdef DEBUG_WEBSOCKETS
		g_warning ("can't yet accept fragmented input");
#endif
	      }
	    else
	      {
		char *terminated = g_strndup((char *)data, payload_len);
	        parse_input_message (input, terminated);
		g_free (terminated);
	      }
	    break;
	  case BROADWAY_WS_CNX_PING:
	    broadway_output_pong (server->output);
	    break;
	  case BROADWAY_WS_CNX_PONG:
	    break; /* we never send pings, but tolerate pongs */
	  case BROADWAY_WS_BINARY:
	  case BROADWAY_WS_CONTINUATION:
	  default:
	    {
	      g_warning ("fragmented or unknown input code 0x%2x with fin set", code);
	      break;
	    }
	  }

	  g_byte_array_remove_range (input->buffer, 0, data - buf + payload_len);
	}
    }
  else /* old style protocol */
    {
      char *buf, *ptr;
      gsize len;

      buf = (char *)input->buffer->data;
      len = input->buffer->len;

      if (buf[0] != 0)
	{
	  server->input = NULL;
	  broadway_input_free (input);
	  return;
	}

      while ((ptr = memchr (buf, 0xff, len)) != NULL)
	{
	  *ptr = 0;
	  ptr++;

	  parse_input_message (input, buf + 1);

	  len -= ptr - buf;
	  buf = ptr;

	  if (len > 0 && buf[0] != 0)
	    {
	      server->input = NULL;
	      broadway_input_free (input);
	      break;
	    }
	}
      g_byte_array_remove_range (input->buffer, 0, buf - (char *)input->buffer->data);
    }
}


static gboolean
process_input_idle_cb (GdkBroadwayServer *server)
{
  server->process_input_idle = 0;
  process_input_messages (server);
  return G_SOURCE_REMOVE;
}

static void
queue_process_input_at_idle (GdkBroadwayServer *server)
{
  if (server->process_input_idle == 0)
    server->process_input_idle =
      g_idle_add_full (G_PRIORITY_DEFAULT, (GSourceFunc)process_input_idle_cb, server, NULL);
}

static void
_gdk_broadway_server_read_all_input_nonblocking (GdkBroadwayServer *server)
{
  GInputStream *in;
  gssize res;
  guint8 buffer[1024];
  GError *error;
  BroadwayInput *input;

  if (server->input == NULL)
    return;

  input = server->input;

  in = g_io_stream_get_input_stream (G_IO_STREAM (input->connection));

  error = NULL;
  res = g_pollable_input_stream_read_nonblocking (G_POLLABLE_INPUT_STREAM (in),
						  buffer, sizeof (buffer), NULL, &error);

  if (res <= 0)
    {
      if (res < 0 &&
	  g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
	{
	  g_error_free (error);
	  return;
	}

      server->input = NULL;
      broadway_input_free (input);
      if (res < 0)
	{
	  g_print ("input error %s\n", error->message);
	  g_error_free (error);
	}
      return;
    }

  g_byte_array_append (input->buffer, buffer, res);

  parse_input (input);
}

static void
_gdk_broadway_server_consume_all_input (GdkBroadwayServer *server)
{
  _gdk_broadway_server_read_all_input_nonblocking (server);

  /* Since we're parsing input but not processing the resulting messages
     we might not get a readable callback on the stream, so queue an idle to
     process the messages */
  queue_process_input_at_idle (server);
}


static gboolean
input_data_cb (GObject  *stream,
	       BroadwayInput *input)
{
  GdkBroadwayServer *server = input->server;

  _gdk_broadway_server_read_all_input_nonblocking (server);

  process_input_messages (server);

  return TRUE;
}

gulong
_gdk_broadway_server_get_next_serial (GdkBroadwayServer *server)
{
  if (server->output)
    return broadway_output_get_next_serial (server->output);

  return server->saved_serial;
}

void
_gdk_broadway_server_flush (GdkBroadwayServer *server)
{
  if (server->output &&
      !broadway_output_flush (server->output))
    {
      server->saved_serial = broadway_output_get_next_serial (server->output);
      broadway_output_free (server->output);
      server->output = NULL;
    }
}

void
_gdk_broadway_server_sync (GdkBroadwayServer *server)
{
  _gdk_broadway_server_flush (server);
}


/* TODO: This is not used atm, is it needed? */
/* Note: This may be called while handling a message (i.e. sorta recursively) */
BroadwayInputMsg *
_gdk_broadway_server_block_for_input (GdkBroadwayServer *server, char op,
				       guint32 serial, gboolean remove_message)
{
  BroadwayInputMsg *message;
  gssize res;
  guint8 buffer[1024];
  BroadwayInput *input;
  GInputStream *in;
  GList *l;

  _gdk_broadway_server_flush (server);

  if (server->input == NULL)
    return NULL;

  input = server->input;

  while (TRUE) {
    /* Check for existing reply in queue */

    for (l = server->input_messages; l != NULL; l = l->next)
      {
	message = l->data;

	if (message->base.type == op)
	  {
	    if (message->base.serial == serial)
	      {
		if (remove_message)
		  server->input_messages =
		    g_list_delete_link (server->input_messages, l);
		return message;
	      }
	  }
      }

    /* Not found, read more, blocking */

    in = g_io_stream_get_input_stream (G_IO_STREAM (input->connection));
    res = g_input_stream_read (in, buffer, sizeof (buffer), NULL, NULL);
    if (res <= 0)
      return NULL;
    g_byte_array_append (input->buffer, buffer, res);

    parse_input (input);

    /* Since we're parsing input but not processing the resulting messages
       we might not get a readable callback on the stream, so queue an idle to
       process the messages */
    queue_process_input_at_idle (server);
  }
}

static char *
parse_line (char *line, char *key)
{
  char *p;

  if (!g_str_has_prefix (line, key))
    return NULL;
  p = line + strlen (key);
  if (*p != ':')
    return NULL;
  p++;
  /* Skip optional initial space */
  if (*p == ' ')
    p++;
  return p;
}
static void
send_error (HttpRequest *request,
	    int error_code,
	    const char *reason)
{
  char *res;

  res = g_strdup_printf ("HTTP/1.0 %d %s\r\n\r\n"
			 "<html><head><title>%d %s</title></head>"
			 "<body>%s</body></html>",
			 error_code, reason,
			 error_code, reason,
			 reason);
  /* TODO: This should really be async */
  g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (request->connection)),
			     res, strlen (res), NULL, NULL, NULL);
  g_free (res);
  http_request_free (request);
}

/* magic from: http://tools.ietf.org/html/draft-ietf-hybi-thewebsocketprotocol-17 */
#define SEC_WEB_SOCKET_KEY_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* 'x3JJHMbDL1EzLkh9GBhXDw==' generates 'HSmrc0sMlYUkAGmm5OPpG2HaGWk=' */
static gchar *
generate_handshake_response_wsietf_v7 (const gchar *key)
{
  gsize digest_len = 20;
  guchar digest[digest_len];
  GChecksum *checksum;

  checksum = g_checksum_new (G_CHECKSUM_SHA1);
  if (!checksum)
    return NULL;

  g_checksum_update (checksum, (guchar *)key, -1);
  g_checksum_update (checksum, (guchar *)SEC_WEB_SOCKET_KEY_MAGIC, -1);

  g_checksum_get_digest (checksum, digest, &digest_len);
  g_checksum_free (checksum);

  g_assert (digest_len == 20);

  return g_base64_encode (digest, digest_len);
}

static void
start_input (HttpRequest *request, gboolean binary)
{
  char **lines;
  char *p;
  int num_key1, num_key2;
  guint64 key1, key2;
  int num_space;
  int i;
  guint8 challenge[16];
  char *res;
  gsize len;
  GChecksum *checksum;
  char *origin, *host;
  GdkBroadwayServer *server;
  BroadwayInput *input;
  const void *data_buffer;
  gsize data_buffer_size;
  GInputStream *in;
  char *key_v7;
  gboolean proto_v7_plus;

  server = GDK_BROADWAY_SERVER (request->server);

#ifdef DEBUG_WEBSOCKETS
  g_print ("incoming request:\n%s\n", request->request->str);
#endif
  lines = g_strsplit (request->request->str, "\n", 0);

  num_key1 = 0;
  num_key2 = 0;
  key1 = 0;
  key2 = 0;
  key_v7 = NULL;
  origin = NULL;
  host = NULL;
  for (i = 0; lines[i] != NULL; i++)
    {
      if ((p = parse_line (lines[i], "Sec-WebSocket-Key1")))
	{
	  num_space = 0;
	  while (*p != 0)
	    {
	      if (g_ascii_isdigit (*p))
		key1 = key1 * 10 + g_ascii_digit_value (*p);
	      else if (*p == ' ')
		num_space++;

	      p++;
	    }
	  key1 /= num_space;
	  num_key1++;
	}
      else if ((p = parse_line (lines[i], "Sec-WebSocket-Key2")))
	{
	  num_space = 0;
	  while (*p != 0)
	    {
	      if (g_ascii_isdigit (*p))
		key2 = key2 * 10 + g_ascii_digit_value (*p);
	      else if (*p == ' ')
		num_space++;

	      p++;
	    }
	  key2 /= num_space;
	  num_key2++;
	}
      else if ((p = parse_line (lines[i], "Sec-WebSocket-Key")))
	{
	  key_v7 = p;
	}
      else if ((p = parse_line (lines[i], "Origin")))
	{
	  origin = p;
	}
      else if ((p = parse_line (lines[i], "Host")))
	{
	  host = p;
	}
      else if ((p = parse_line (lines[i], "Sec-WebSocket-Origin")))
	{
	  origin = p;
	}
    }

  if (origin == NULL || host == NULL)
    {
      g_strfreev (lines);
      send_error (request, 400, "Bad websocket request");
      return;
    }

  if (key_v7 != NULL)
    {
      char* accept = generate_handshake_response_wsietf_v7 (key_v7);
      res = g_strdup_printf ("HTTP/1.1 101 Switching Protocols\r\n"
			     "Upgrade: websocket\r\n"
			     "Connection: Upgrade\r\n"
			     "Sec-WebSocket-Accept: %s\r\n"
			     "Sec-WebSocket-Origin: %s\r\n"
			     "Sec-WebSocket-Location: ws://%s/socket\r\n"
			     "Sec-WebSocket-Protocol: broadway\r\n"
			     "\r\n", accept, origin, host);
      g_free (accept);

#ifdef DEBUG_WEBSOCKETS
      g_print ("v7 proto response:\n%s", res);
#endif

      g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (request->connection)),
				 res, strlen (res), NULL, NULL, NULL);
      g_free (res);
      proto_v7_plus = TRUE;
    }
  else
    {
      if (num_key1 != 1 || num_key2 != 1)
	{
	  g_strfreev (lines);
	  send_error (request, 400, "Bad websocket request");
	  return;
	}

      challenge[0] = (key1 >> 24) & 0xff;
      challenge[1] = (key1 >> 16) & 0xff;
      challenge[2] = (key1 >>  8) & 0xff;
      challenge[3] = (key1 >>  0) & 0xff;
      challenge[4] = (key2 >> 24) & 0xff;
      challenge[5] = (key2 >> 16) & 0xff;
      challenge[6] = (key2 >>  8) & 0xff;
      challenge[7] = (key2 >>  0) & 0xff;

      if (!g_input_stream_read_all (G_INPUT_STREAM (request->data), challenge+8, 8, NULL, NULL, NULL))
	{
	  g_strfreev (lines);
	  send_error (request, 400, "Bad websocket request");
	  return;
	}

      checksum = g_checksum_new (G_CHECKSUM_MD5);
      g_checksum_update (checksum, challenge, 16);
      len = 16;
      g_checksum_get_digest (checksum, challenge, &len);
      g_checksum_free (checksum);

      res = g_strdup_printf ("HTTP/1.1 101 WebSocket Protocol Handshake\r\n"
			     "Upgrade: WebSocket\r\n"
			     "Connection: Upgrade\r\n"
			     "Sec-WebSocket-Origin: %s\r\n"
			     "Sec-WebSocket-Location: ws://%s/socket\r\n"
			     "Sec-WebSocket-Protocol: broadway\r\n"
			     "\r\n",
			     origin, host);

#ifdef DEBUG_WEBSOCKETS
      g_print ("legacy response:\n%s", res);
#endif
      g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (request->connection)),
				 res, strlen (res), NULL, NULL, NULL);
      g_free (res);
      g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (request->connection)),
				 challenge, 16, NULL, NULL, NULL);
      proto_v7_plus = FALSE;
    }


  if (server->input != NULL)
    {
      broadway_input_free (server->input);
      server->input = NULL;
    }

  input = g_new0 (BroadwayInput, 1);

  input->server = request->server;
  input->connection = g_object_ref (request->connection);
  input->proto_v7_plus = proto_v7_plus;
  input->binary = binary;

  data_buffer = g_buffered_input_stream_peek_buffer (G_BUFFERED_INPUT_STREAM (request->data), &data_buffer_size);
  input->buffer = g_byte_array_sized_new (data_buffer_size);
  g_byte_array_append (input->buffer, data_buffer, data_buffer_size);

  server->input = input;

  start_output (request, proto_v7_plus, binary);

  /* This will free and close the data input stream, but we got all the buffered content already */
  http_request_free (request);

  in = g_io_stream_get_input_stream (G_IO_STREAM (input->connection));
  input->source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (in), NULL);
  g_source_set_callback (input->source, (GSourceFunc)input_data_cb, input, NULL);
  g_source_attach (input->source, NULL);

  /* Process any data in the pipe already */
  parse_input (input);
  process_input_messages (server);

  g_strfreev (lines);
}

static void
start_output (HttpRequest *request, gboolean proto_v7_plus, gboolean binary)
{
  GSocket *socket;
  GdkBroadwayServer *server;
  int flag = 1;

  socket = g_socket_connection_get_socket (request->connection);
  setsockopt(g_socket_get_fd (socket), IPPROTO_TCP,
	     TCP_NODELAY, (char *) &flag, sizeof(int));

  server = GDK_BROADWAY_SERVER (request->server);

  if (server->output)
    {
      server->saved_serial = broadway_output_get_next_serial (server->output);
      broadway_output_free (server->output);
    }

  server->output =
    broadway_output_new (g_io_stream_get_output_stream (G_IO_STREAM (request->connection)),
			 server->saved_serial, proto_v7_plus, binary);

  _gdk_broadway_server_resync_windows (server);

  if (server->pointer_grab_window_id != -1)
    broadway_output_grab_pointer (server->output,
				  server->pointer_grab_window_id,
				  server->pointer_grab_owner_events);
}

static void
send_data (HttpRequest *request,
	     const char *mimetype,
	     const char *data, gsize len)
{
  char *res;

  res = g_strdup_printf ("HTTP/1.0 200 OK\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %"G_GSIZE_FORMAT"\r\n"
			 "\r\n",
			 mimetype, len);
  /* TODO: This should really be async */
  g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (request->connection)),
			     res, strlen (res), NULL, NULL, NULL);
  g_free (res);
  g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (request->connection)),
			     data, len, NULL, NULL, NULL);
  http_request_free (request);
}

#include "clienthtml.h"
#include "broadwayjs.h"

static void
got_request (HttpRequest *request)
{
  char *start, *escaped, *tmp, *version, *query;

  if (!g_str_has_prefix (request->request->str, "GET "))
    {
      send_error (request, 501, "Only GET implemented");
      return;
    }

  start = request->request->str + 4; /* Skip "GET " */

  while (*start == ' ')
    start++;

  for (tmp = start; *tmp != 0 && *tmp != ' ' && *tmp != '\n'; tmp++)
    ;
  escaped = g_strndup (start, tmp - start);
  version = NULL;
  if (*tmp == ' ')
    {
      start = tmp;
      while (*start == ' ')
	start++;
      for (tmp = start; *tmp != 0 && *tmp != ' ' && *tmp != '\n'; tmp++)
	;
      version = g_strndup (start, tmp - start);
    }

  query = strchr (escaped, '?');
  if (query)
    *query = 0;

  if (strcmp (escaped, "/client.html") == 0 || strcmp (escaped, "/") == 0)
    send_data (request, "text/html", client_html, G_N_ELEMENTS(client_html) - 1);
  else if (strcmp (escaped, "/broadway.js") == 0)
    send_data (request, "text/javascript", broadway_js, G_N_ELEMENTS(broadway_js) - 1);
  else if (strcmp (escaped, "/socket") == 0)
    start_input (request, FALSE);
  else if (strcmp (escaped, "/socket-bin") == 0)
    start_input (request, TRUE);
  else
    send_error (request, 404, "File not found");

  g_free (escaped);
  g_free (version);
}

static void
got_http_request_line (GInputStream *stream,
		       GAsyncResult *result,
		       HttpRequest *request)
{
  char *line;

  line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (stream), result, NULL, NULL);
  if (line == NULL)
    {
      http_request_free (request);
      g_printerr ("Error reading request lines\n");
      return;
    }
  if (strlen (line) == 0)
    got_request (request);
  else
    {
      /* Protect against overflow in request length */
      if (request->request->len > 1024 * 5)
	{
	  send_error (request, 400, "Request too long");
	}
      else
	{
	  g_string_append_printf (request->request, "%s\n", line);
	  g_data_input_stream_read_line_async (request->data, 0, NULL,
					       (GAsyncReadyCallback)got_http_request_line, request);
	}
    }
  g_free (line);
}

static gboolean
handle_incoming_connection (GSocketService    *service,
			    GSocketConnection *connection,
			    GObject           *source_object)
{
  HttpRequest *request;
  GInputStream *in;

  request = g_new0 (HttpRequest, 1);
  request->connection = g_object_ref (connection);
  request->server = GDK_BROADWAY_SERVER (source_object);
  request->request = g_string_new ("");

  in = g_io_stream_get_input_stream (G_IO_STREAM (connection));

  request->data = g_data_input_stream_new (in);
  g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (request->data), FALSE);
  /* Be tolerant of input */
  g_data_input_stream_set_newline_type (request->data, G_DATA_STREAM_NEWLINE_TYPE_ANY);

  g_data_input_stream_read_line_async (request->data, 0, NULL,
				       (GAsyncReadyCallback)got_http_request_line, request);
  return TRUE;
}

GdkBroadwayServer *
_gdk_broadway_server_new (int port, GError **error)
{
  GdkBroadwayServer *server;

  server = g_object_new (GDK_TYPE_BROADWAY_SERVER, NULL);
  server->port = port;

  if (!g_socket_listener_add_inet_port (G_SOCKET_LISTENER (server->service),
					server->port,
					G_OBJECT (server),
					error))
    {
      g_prefix_error (error, "Unable to listen to port %d: ", server->port);
      return NULL;
    }

  g_signal_connect (server->service, "incoming",
		    G_CALLBACK (handle_incoming_connection), NULL);
  return server;
}

guint32
_gdk_broadway_server_get_last_seen_time (GdkBroadwayServer *server)
{
  _gdk_broadway_server_consume_all_input (server);
  return (guint32) server->last_seen_time;
}

void
_gdk_broadway_server_query_mouse (GdkBroadwayServer *server,
				  gint32             *toplevel,
				  gint32             *root_x,
				  gint32             *root_y,
				  guint32            *mask)
{
  if (server->output)
    {
      _gdk_broadway_server_consume_all_input (server);
      if (root_x)
	*root_x = server->future_root_x;
      if (root_y)
	*root_y = server->future_root_y;
      if (mask)
	*mask = server->future_state;
      if (toplevel)
	*toplevel = server->future_mouse_in_toplevel;
      return;
    }

  /* Fallback when unconnected */
  if (root_x)
    *root_x = server->last_x;
  if (root_y)
    *root_y = server->last_y;
  if (mask)
    *mask = server->last_state;
  if (toplevel)
    *toplevel = server->mouse_in_toplevel_id;
}

void
_gdk_broadway_server_destroy_window (GdkBroadwayServer *server,
				     gint id)
{
  BroadwayWindow *window;

  if (server->mouse_in_toplevel_id == id)
    {
      /* TODO: Send leave + enter event, update cursors, etc */
      server->mouse_in_toplevel_id = 0;
    }

  if (server->pointer_grab_window_id == id)
    server->pointer_grab_window_id = -1;

  if (server->output)
    broadway_output_destroy_surface (server->output,
				     id);

  window = g_hash_table_lookup (server->id_ht,
				GINT_TO_POINTER (id));
  if (window != NULL)
    {
      server->toplevels = g_list_remove (server->toplevels, window);
      g_hash_table_remove (server->id_ht,
			   GINT_TO_POINTER (id));
      g_free (window);
    }
}

gboolean
_gdk_broadway_server_window_show (GdkBroadwayServer *server,
				  gint id)
{
  BroadwayWindow *window;
  gboolean sent = FALSE;

  window = g_hash_table_lookup (server->id_ht,
				GINT_TO_POINTER (id));
  if (window == NULL)
    return FALSE;

  window->visible = TRUE;

  if (server->output)
    {
      broadway_output_show_surface (server->output, window->id);
      sent = TRUE;
    }

  return sent;
}

gboolean
_gdk_broadway_server_window_hide (GdkBroadwayServer *server,
				  gint id)
{
  BroadwayWindow *window;
  gboolean sent = FALSE;

  window = g_hash_table_lookup (server->id_ht,
				GINT_TO_POINTER (id));
  if (window == NULL)
    return FALSE;

  window->visible = FALSE;

  if (server->mouse_in_toplevel_id == id)
    {
      /* TODO: Send leave + enter event, update cursors, etc */
      server->mouse_in_toplevel_id = 0;
    }

  if (server->output)
    {
      broadway_output_hide_surface (server->output, window->id);
      sent = TRUE;
    }
  return sent;
}

void
_gdk_broadway_server_window_set_transient_for (GdkBroadwayServer *server,
					       gint id, gint parent)
{
  BroadwayWindow *window;

  window = g_hash_table_lookup (server->id_ht,
				GINT_TO_POINTER (id));
  if (window == NULL)
    return;

  window->transient_for = parent;

  if (server->output)
    {
      broadway_output_set_transient_for (server->output, window->id, window->transient_for);
      _gdk_broadway_server_flush (server);
    }
}

gboolean
_gdk_broadway_server_has_client (GdkBroadwayServer *server)
{
  return server->output != NULL;
}

static void
_cairo_region (cairo_t         *cr,
	       const cairo_region_t *region)
{
  cairo_rectangle_int_t box;
  gint n_boxes, i;

  g_return_if_fail (cr != NULL);
  g_return_if_fail (region != NULL);

  n_boxes = cairo_region_num_rectangles (region);

  for (i = 0; i < n_boxes; i++)
    {
      cairo_region_get_rectangle (region, i, &box);
      cairo_rectangle (cr, box.x, box.y, box.width, box.height);
    }
}


static void
copy_region (cairo_surface_t *surface,
	     cairo_region_t *area,
	     gint            dx,
	     gint            dy)
{
  cairo_t *cr;

  cr = cairo_create (surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);

  _cairo_region (cr, area);
  cairo_clip (cr);

  /* NB: This is a self-copy and Cairo doesn't support that yet.
   * So we do a litle trick.
   */
  cairo_push_group (cr);

  cairo_set_source_surface (cr, surface, dx, dy);
  cairo_paint (cr);

  cairo_pop_group_to_source (cr);
  cairo_paint (cr);

  cairo_destroy (cr);
}

gboolean
_gdk_broadway_server_window_translate (GdkBroadwayServer *server,
				       gint id,
				       cairo_region_t *area,
				       gint            dx,
				       gint            dy)
{
  BroadwayWindow *window;
  gboolean sent = FALSE;

  window = g_hash_table_lookup (server->id_ht,
				GINT_TO_POINTER (id));
  if (window == NULL)
    return FALSE;

  if (window->last_synced &&
      server->output)
    {
      BroadwayRect *rects;
      cairo_rectangle_int_t rect;
      int i, n_rects;

      copy_region (window->last_surface, area, dx, dy);
      n_rects = cairo_region_num_rectangles (area);
      rects = g_new (BroadwayRect, n_rects);
      for (i = 0; i < n_rects; i++)
	{
	  cairo_region_get_rectangle (area, i, &rect);
	  rects[i].x = rect.x;
	  rects[i].y = rect.y;
	  rects[i].width = rect.width;
	  rects[i].height = rect.height;
	}
      broadway_output_copy_rectangles (server->output,
				       window->id,
				       rects, n_rects, dx, dy);
      g_free (rects);
      sent = TRUE;
    }

  return sent;
}

static void
diff_surfaces (cairo_surface_t *surface,
	       cairo_surface_t *old_surface)
{
  guint8 *data, *old_data;
  guint32 *line, *old_line;
  int w, h, stride, old_stride;
  int x, y;

  data = cairo_image_surface_get_data (surface);
  old_data = cairo_image_surface_get_data (old_surface);

  w = cairo_image_surface_get_width (surface);
  h = cairo_image_surface_get_height (surface);

  stride = cairo_image_surface_get_stride (surface);
  old_stride = cairo_image_surface_get_stride (old_surface);

  for (y = 0; y < h; y++)
    {
      line = (guint32 *)data;
      old_line = (guint32 *)old_data;

      for (x = 0; x < w; x++)
	{
	  if ((*line & 0xffffff) == (*old_line & 0xffffff))
	    *old_line = 0;
	  else
	    *old_line = *line | 0xff000000;
	  line ++;
	  old_line ++;
	}

      data += stride;
      old_data += old_stride;
    }
}

void
_gdk_broadway_server_window_update (GdkBroadwayServer *server,
				    gint id,
				    cairo_surface_t *surface)
{
  cairo_t *cr;
  BroadwayWindow *window;

  if (surface == NULL)
    return;

  window = g_hash_table_lookup (server->id_ht,
				GINT_TO_POINTER (id));
  if (window == NULL)
    return;

  if (window->last_surface == NULL)
    window->last_surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24,
						       window->width,
						       window->height);

  if (server->output != NULL)
    {
      if (window->last_synced)
	{
	  diff_surfaces (surface,
			 window->last_surface);
	  broadway_output_put_rgba (server->output, window->id, 0, 0,
				    cairo_image_surface_get_width (window->last_surface),
				    cairo_image_surface_get_height (window->last_surface),
				    cairo_image_surface_get_stride (window->last_surface),
				    cairo_image_surface_get_data (window->last_surface));
	}
      else
	{
	  window->last_synced = TRUE;
	  broadway_output_put_rgb (server->output, window->id, 0, 0,
				   cairo_image_surface_get_width (surface),
				   cairo_image_surface_get_height (surface),
				   cairo_image_surface_get_stride (surface),
				   cairo_image_surface_get_data (surface));
	}

      broadway_output_surface_flush (server->output, window->id);
    }

  cr = cairo_create (window->last_surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);
}

gboolean
_gdk_broadway_server_window_move_resize (GdkBroadwayServer *server,
					 gint id,
					 int x,
					 int y,
					 int width,
					 int height)
{
  BroadwayWindow *window;
  gboolean with_move, with_resize;
  gboolean sent = FALSE;
  cairo_t *cr;

  window = g_hash_table_lookup (server->id_ht,
				GINT_TO_POINTER (id));
  if (window == NULL)
    return FALSE;

  with_move = x != window->x || y != window->y;
  with_resize = width != window->width || height != window->height;
  window->x = x;
  window->y = y;
  window->width = width;
  window->height = height;

  if (with_resize && window->last_surface != NULL)
    {
      cairo_surface_t *old;

      old = window->last_surface;

      window->last_surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24,
							 width, height);


      cr = cairo_create (window->last_surface);
      cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_surface (cr, old, 0, 0);
      cairo_paint (cr);
      cairo_destroy (cr);

      cairo_surface_destroy (old);
    }

  if (server->output != NULL)
    {
      broadway_output_move_resize_surface (server->output,
					   window->id,
					   with_move, window->x, window->y,
					   with_resize, window->width, window->height);
      sent = TRUE;
    }

  return sent;
}

GdkGrabStatus
_gdk_broadway_server_grab_pointer (GdkBroadwayServer *server,
				   gint id,
				   gboolean owner_events,
				   guint32 event_mask,
				   guint32 time_)
{
  if (server->pointer_grab_window_id != -1 &&
      time_ != 0 && server->pointer_grab_time > time_)
    return GDK_GRAB_ALREADY_GRABBED;

  if (time_ == 0)
    time_ = server->last_seen_time;

  server->pointer_grab_window_id = id;
  server->pointer_grab_owner_events = owner_events;
  server->pointer_grab_time = time_;

  if (server->output)
    {
      broadway_output_grab_pointer (server->output,
				    id,
				    owner_events);
      _gdk_broadway_server_flush (server);
    }

  /* TODO: What about toplevel grab events if we're not connected? */

  return GDK_GRAB_SUCCESS;
}

guint32
_gdk_broadway_server_ungrab_pointer (GdkBroadwayServer *server,
				     guint32    time_)
{
  guint32 serial;

  if (server->pointer_grab_window_id != -1 &&
      time_ != 0 && server->pointer_grab_time > time_)
    return 0;

  /* TODO: What about toplevel grab events if we're not connected? */

  if (server->output)
    {
      serial = broadway_output_ungrab_pointer (server->output);
      _gdk_broadway_server_flush (server);
    }
  else
    {
      serial = server->saved_serial;
    }

  server->pointer_grab_window_id = -1;

  return serial;
}

guint32
_gdk_broadway_server_new_window (GdkBroadwayServer *server,
				 int x,
				 int y,
				 int width,
				 int height,
				 gboolean is_temp)
{
  BroadwayWindow *window;

  window = g_new0 (BroadwayWindow, 1);
  window->id = server->id_counter++;
  window->x = x;
  window->y = y;
  window->width = width;
  window->height = height;
  window->is_temp = is_temp;

  g_hash_table_insert (server->id_ht,
		       GINT_TO_POINTER (window->id),
		       window);

  server->toplevels = g_list_prepend (server->toplevels, window);

  if (server->output)
    broadway_output_new_surface (server->output,
				 window->id,
				 window->x,
				 window->y,
				 window->width,
				 window->height,
				 window->is_temp);

  return window->id;
}

static void
_gdk_broadway_server_resync_windows (GdkBroadwayServer *server)
{
  GList *l;

  if (server->output == NULL)
    return;

  /* First create all windows */
  for (l = server->toplevels; l != NULL; l = l->next)
    {
      BroadwayWindow *window = l->data;

      if (window->id == 0)
	continue; /* Skip root */

      window->last_synced = FALSE;
      broadway_output_new_surface (server->output,
				   window->id,
				   window->x,
				   window->y,
				   window->width,
				   window->height,
				   window->is_temp);
    }

  /* Then do everything that may reference other windows */
  for (l = server->toplevels; l != NULL; l = l->next)
    {
      BroadwayWindow *window = l->data;

      if (window->id == 0)
	continue; /* Skip root */

      if (window->transient_for != -1)
	broadway_output_set_transient_for (server->output, window->id, window->transient_for);
      if (window->visible)
	{
	  broadway_output_show_surface (server->output, window->id);

	  if (window->last_surface != NULL)
	    {
	      window->last_synced = TRUE;
	      broadway_output_put_rgb (server->output, window->id, 0, 0,
				       cairo_image_surface_get_width (window->last_surface),
				       cairo_image_surface_get_height (window->last_surface),
				       cairo_image_surface_get_stride (window->last_surface),
				       cairo_image_surface_get_data (window->last_surface));
	    }
	}
    }

  _gdk_broadway_server_flush (server);
}