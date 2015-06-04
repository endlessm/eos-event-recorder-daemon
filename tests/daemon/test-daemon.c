/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014, 2015 Endless Mobile, Inc. */

/*
 * This file is part of eos-event-recorder-daemon.
 *
 * eos-event-recorder-daemon is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 *
 * eos-event-recorder-daemon is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with eos-event-recorder-daemon.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "emer-daemon.h"
#include "emer-boot-id-provider.h"
#include "emer-machine-id-provider.h"
#include "emer-network-send-provider.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"
#include "mock-permissions-provider.h"
#include "mock-persistent-cache.h"
#include "shared/metrics-util.h"

#include <eosmetrics/eosmetrics.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <uuid/uuid.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#define MOCK_SERVER_PATH TEST_DIR "daemon/mock-server.py"

#define MEANINGLESS_EVENT "350ac4ff-3026-4c25-9e7e-e8103b4fd5d8"
#define MEANINGLESS_EVENT_2 "d936cd5c-08de-4d4e-8a87-8df1f4a33cba"

#define USER_ID 4200u
#define NUM_EVENTS 101
#define RELATIVE_TIMESTAMP G_GINT64_CONSTANT (123456789)
#define OFFSET_TIMESTAMP (RELATIVE_TIMESTAMP + BOOT_TIME_OFFSET)

#define TIMEOUT_SEC 5

#define EXPECTED_INHIBIT_SHUTDOWN_ARGS \
  "\"shutdown\" " \
  "\"EndlessOS Event Recorder Daemon\" " \
  "\"Flushing events to disk\" " \
  "\"delay\""

typedef struct _Fixture
{
  EmerDaemon *test_object;
  EmerMachineIdProvider *mock_machine_id_provider;
  EmerNetworkSendProvider *mock_network_send_provider;
  EmerPermissionsProvider *mock_permissions_provider;
  EmerPersistentCache *mock_persistent_cache;

  GSubprocess *mock_server;
  GSubprocess *logind_mock;

  gint64 relative_time;
  gint64 absolute_time;
  gchar *request_path;
} Fixture;

typedef void (*ProcessBytesSourceFunc) (GByteArray *, gpointer);
typedef gboolean (*ProcessLineSourceFunc) (GString *, gpointer);

typedef struct _ByteCollector
{
  GMainLoop *main_loop;
  GByteArray *byte_array;
  guint num_bytes_to_collect;
  ProcessBytesSourceFunc source_func;
  gpointer user_data;
} ByteCollector;

typedef struct _LineCollector
{
  GMainLoop *main_loop;
  GString *line;
  ProcessLineSourceFunc source_func;
  gpointer user_data;
} LineCollector;

typedef struct _UploadEventsCallbackData
{
  GMainLoop *main_loop;
  GSubprocess *mock_server;
} UploadEventsCallbackData;

// Helper methods first:

static void
terminate_subprocess_and_wait (GSubprocess *subprocess)
{
  g_subprocess_send_signal (subprocess, SIGTERM);

  /*
   * Make sure it was the SIGTERM that finished the process, and not something
   * else.
   */
  GError *error = NULL;
  g_assert_false (g_subprocess_wait_check (subprocess, NULL, &error));
  g_assert_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "Child process killed by signal 15");

  g_object_unref (subprocess);
}

static void
start_mock_logind_service (Fixture *fixture)
{
  fixture->logind_mock =
    g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL, "python3", "-m",
                      "dbusmock", "--system", "--template", "logind", NULL);
  g_assert_nonnull (fixture->logind_mock);
}

static gboolean
timeout (gpointer unused)
{
  g_assert_not_reached ();
}

/*
 * Looks through the given @line of a mock DBus process' output for a call
 * matching @method_name and containing the string @arguments in its arguments.
 *
 * Returns %TRUE if the call was found in @line and @arguments matched.
 *
 * Returns %FALSE if the call was not found in @line, or the call was found but
 * @arguments was given and did not match.
 */
static gboolean
contains_dbus_call (const gchar *line,
                    const gchar *method_name,
                    const gchar *arguments)
{
  gchar *method_called = NULL, *arguments_given = NULL;
  if (sscanf (line, "%*f %ms %m[^\n]", &method_called, &arguments_given) != 2)
    {
      g_free (method_called);
      return FALSE;
    }

  if (strcmp (method_name, method_called) != 0)
    {
      g_free (method_called);
      g_free (arguments_given);
      return FALSE;
    }
  g_free (method_called);

  gchar *given_args_index = strstr (arguments_given, arguments);
  g_free (arguments_given);
  return given_args_index != NULL;
}

static gboolean
process_logind_line (GString *line,
                     gpointer unused)
{
  // Ensure that the only null byte in the line is the terminal null byte.
  g_assert_cmpuint (line->len, ==, strlen (line->str));

  return !contains_dbus_call (line->str, "Inhibit",
                              EXPECTED_INHIBIT_SHUTDOWN_ARGS);
}

static gboolean
remove_last_character (GString *line,
                       gchar  **stripped_line)
{
  *stripped_line = g_strndup (line->str, line->len - 1);
  return G_SOURCE_REMOVE;
}

static gboolean
read_content_length (GString *line,
                     guint   *content_length)
{
  gint num_conversions = sscanf (line->str, "%u\n", content_length);
  g_assert_cmpint (num_conversions, ==, 1);
  return G_SOURCE_REMOVE;
}

static GPollableInputStream *
get_pollable_input_stream (GSubprocess *subprocess)
{
  GInputStream *input_stream = g_subprocess_get_stdout_pipe (subprocess);
  GPollableInputStream *pollable_input_stream =
    G_POLLABLE_INPUT_STREAM (input_stream);
  g_assert_true (g_pollable_input_stream_can_poll (pollable_input_stream));
  return pollable_input_stream;
}

/* Read 1 byte from the given stream without blocking. Returns TRUE if a byte
 * was successfully read. Returns FALSE if a byte can't be obtained from the
 * given stream without blocking. Pass NULL for byte to ignore the byte read
 * from the stream.
 */
static gboolean
read_byte (GPollableInputStream *pollable_input_stream,
           guint8               *byte)
{
  guint8 byte_read;
  GError *error = NULL;
  gssize num_bytes_read =
    g_pollable_input_stream_read_nonblocking (pollable_input_stream, &byte_read,
                                              1, NULL /* GCancellable */,
                                              &error);
  switch (num_bytes_read)
    {
    case -1:
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
      g_error_free (error); // Fall through.
    case 0:
      return FALSE;
    case 1:
      if (byte != NULL)
        *byte = byte_read;
      return TRUE;
    default:
      g_assert_not_reached ();
    }
}

/* Appends 1 character from the given stream to the given string without
 * blocking. Returns TRUE if a character was successfully appended. Returns
 * FALSE if a character can't be obtained from the given stream without
 * blocking.
 */
static gboolean
append_char (GPollableInputStream *pollable_input_stream,
             GString              *string)
{
  guint8 character;
  if (!read_byte (pollable_input_stream, &character))
    return FALSE;

  g_string_append_c (string, character);
  return TRUE;
}

/* Appends 1 line from the given stream to the given string without blocking.
 * Returns TRUE if a full line was successfully appended. Returns FALSE if less
 * than 1 line can be obtained from the given stream without blocking, appending
 * whatever data was available for immediate consumption.
 */
static gboolean
append_line (GPollableInputStream *pollable_input_stream,
             GString              *line)
{
  while (append_char (pollable_input_stream, line))
    {
      if (line->str[line->len - 1] == '\n')
        return TRUE;
    }

  return FALSE;
}

/* Appends bytes from the given stream to the given byte array. Returns TRUE
 * once the given string reaches the given length. Returns FALSE if insufficient
 * data can be obtained from the given stream without blocking, appending
 * whatever data was available for immediate consumption. Assumes byte_array
 * already has space for the given number of bytes. See g_byte_array_sized_new.
 */
static gboolean
append_bytes (GPollableInputStream *pollable_input_stream,
              GByteArray           *byte_array,
              guint                 num_bytes_to_collect)
{
  guint num_bytes_remaining = num_bytes_to_collect - byte_array->len;
  guint8 *destination = byte_array->data + byte_array->len;
  GError *error = NULL;
  gssize num_bytes_read =
    g_pollable_input_stream_read_nonblocking (pollable_input_stream,
                                              destination, num_bytes_remaining,
                                              NULL /* GCancellable */, &error);
  if (num_bytes_read == -1)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
      g_error_free (error);
      return FALSE;
    }

  guint new_length = byte_array->len + num_bytes_read;
  g_byte_array_set_size (byte_array, new_length);
  return num_bytes_read == num_bytes_remaining;
}

static gboolean
collect_lines (GPollableInputStream  *pollable_input_stream,
               LineCollector         *line_collector)
{
  while (append_line (pollable_input_stream, line_collector->line))
    {
      gboolean continue_listening =
        line_collector->source_func (line_collector->line,
                                     line_collector->user_data);
      if (!continue_listening)
        {
          g_main_loop_quit (line_collector->main_loop);
          return G_SOURCE_REMOVE;
        }

      g_string_truncate (line_collector->line, 0);
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
collect_bytes (GPollableInputStream  *pollable_input_stream,
               ByteCollector         *byte_collector)
{
  if (!append_bytes (pollable_input_stream, byte_collector->byte_array,
                     byte_collector->num_bytes_to_collect))
    return G_SOURCE_CONTINUE;

  byte_collector->source_func (byte_collector->byte_array,
                               byte_collector->user_data);

  g_main_loop_quit (byte_collector->main_loop);
  return G_SOURCE_REMOVE;
}

/* Reads line by line from stdout of the given subprocess, blocking until either
 * the desired data becomes available or a timeout expires. Calls source_func
 * with a GString containing a line, including the terminal newline character,
 * as the first parameter. Passes the given callback data as the second
 * parameter. Each time source_func returns G_SOURCE_CONTINUE, another line is
 * read and passed to source_func. This function only returns once source_func
 * returns G_SOURCE_REMOVE.
 */
static void
read_lines_from_stdout (GSubprocess          *subprocess,
                        ProcessLineSourceFunc source_func,
                        gpointer              user_data)
{
  GPollableInputStream *pollable_input_stream =
    get_pollable_input_stream (subprocess);

  GSource *stdout_source =
    g_pollable_input_stream_create_source (pollable_input_stream,
                                           NULL /* GCancellable */);

  LineCollector line_collector =
    {
      g_main_loop_new (NULL /* GMainContext */, FALSE),
      g_string_new (""),
      source_func,
      user_data,
    };

  g_source_set_callback (stdout_source, (GSourceFunc) collect_lines,
                         &line_collector, NULL /* GDestroyNotify */);
  g_source_attach (stdout_source, NULL /* GMainContext */);
  g_source_unref (stdout_source);

  guint timeout_id =
    g_timeout_add_seconds (TIMEOUT_SEC, timeout, NULL /* user data */);

  g_main_loop_run (line_collector.main_loop);

  g_source_remove (timeout_id);
  g_main_loop_unref (line_collector.main_loop);
  g_string_free (line_collector.line, TRUE);
}

/* Reads the given number of bytes from stdout of the given subprocess, blocking
 * until either sufficient data becomes available or a timeout expires. Calls
 * source_func with a GByteArray containing the bytes read as the first
 * parameter and the given callback data as the second parameter.
 */
static void
read_bytes_from_stdout (GSubprocess           *subprocess,
                        guint                  num_bytes,
                        ProcessBytesSourceFunc source_func,
                        gpointer               user_data)
{
  GPollableInputStream *pollable_input_stream =
    get_pollable_input_stream (subprocess);

  GSource *stdout_source =
    g_pollable_input_stream_create_source (pollable_input_stream,
                                           NULL /* GCancellable */);

  ByteCollector byte_collector =
    {
      g_main_loop_new (NULL /* GMainContext */, FALSE),
      g_byte_array_sized_new (num_bytes),
      num_bytes,
      source_func,
      user_data,
    };

  g_source_set_callback (stdout_source, (GSourceFunc) collect_bytes,
                         &byte_collector, NULL /* GDestroyNotify */);
  g_source_attach (stdout_source, NULL /* GMainContext */);
  g_source_unref (stdout_source);

  guint timeout_id =
    g_timeout_add_seconds (TIMEOUT_SEC, timeout, NULL /* user data */);

  g_main_loop_run (byte_collector.main_loop);

  g_source_remove (timeout_id);
  g_main_loop_unref (byte_collector.main_loop);
  g_byte_array_unref (byte_collector.byte_array);
}

static void
emit_shutdown_signal (gboolean shutdown)
{
  GDBusConnection *system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
  g_assert_nonnull (system_bus);

  GVariantBuilder args_builder;
  g_variant_builder_init (&args_builder, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&args_builder, "v", g_variant_new ("b", shutdown));
  GVariant *response =
    g_dbus_connection_call_sync (system_bus, "org.freedesktop.login1",
                                 "/org/freedesktop/login1",
                                 "org.freedesktop.DBus.Mock", "EmitSignal",
                                 g_variant_new ("(sssav)", "",
                                                "PrepareForShutdown", "b",
                                                &args_builder),
                                 NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                 TIMEOUT_SEC * 1000,
                                 NULL, NULL);
  g_assert_nonnull (response);
}

static GVariant *
make_event_id_variant (void)
{
  uuid_t uuid;
  g_assert_cmpint (uuid_parse (MEANINGLESS_EVENT, uuid), ==, 0);
  GVariantBuilder event_id_builder;
  get_uuid_builder (uuid, &event_id_builder);
  return g_variant_builder_end (&event_id_builder);
}

static GVariant *
make_auxiliary_payload (void)
{
  GVariant *sword_of_a_thousand = g_variant_new_boolean (TRUE);
  return g_variant_new_variant (sword_of_a_thousand);
}

static GVariant *
make_event_values_variant (void)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(xbv)"));
  g_variant_builder_add (&builder,
                         "(xbv)",
                         RELATIVE_TIMESTAMP,
                         FALSE,
                         g_variant_new_boolean (FALSE));
  g_variant_builder_add (&builder,
                         "(xbv)",
                         RELATIVE_TIMESTAMP,
                         TRUE,
                         g_variant_new_boolean (TRUE));
  return g_variant_builder_end (&builder);
}

static void
assert_no_data_uploaded (EmerDaemon               *daemon,
                         GAsyncResult             *result,
                         UploadEventsCallbackData *callback_data)
{
  GError *error = NULL;
  g_assert_false (emer_daemon_upload_events_finish (daemon, result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_error_free (error);

  GPollableInputStream *pollable_input_stream =
    get_pollable_input_stream (callback_data->mock_server);
  g_assert_false (read_byte (pollable_input_stream, NULL /* byte */));

  g_main_loop_quit (callback_data->main_loop);
}

static void
assert_uploading_disabled (Fixture *fixture)
{
  UploadEventsCallbackData callback_data =
    {
      g_main_loop_new (NULL /* GMainContext */, FALSE),
      fixture->mock_server,
    };

  emer_daemon_upload_events (fixture->test_object,
                             (GAsyncReadyCallback) assert_no_data_uploaded,
                             &callback_data);

  guint timeout_id =
    g_timeout_add_seconds (TIMEOUT_SEC, timeout, NULL /* user data */);

  g_main_loop_run (callback_data.main_loop);

  g_source_remove (timeout_id);
  g_main_loop_unref (callback_data.main_loop);
}

static void
assert_variants_equal (GVariant *variant_one,
                       GVariant *variant_two)
{
  if (variant_one == NULL)
    {
      g_assert_null (variant_two);
    }
  else
    {
      g_assert_nonnull (variant_two);
      g_assert_true (g_variant_equal (variant_one, variant_two));
    }
}

static void assert_machine_id_matches (GVariant              *machine_id_variant,
                                       EmerMachineIdProvider *machine_id_provider)
{
  gsize actual_length;
  const guchar *actual_machine_id =
    g_variant_get_fixed_array (machine_id_variant, &actual_length,
                               sizeof (guchar));
  g_assert_cmpuint (actual_length, ==, UUID_LENGTH);

  uuid_t expected_machine_id;
  emer_machine_id_provider_get_id (machine_id_provider, expected_machine_id);

  gint compare_result = uuid_compare (actual_machine_id, expected_machine_id);
  g_assert_cmpint (compare_result, ==, 0);
}


static void
assert_singular_matches (GVariantIter *singular_iterator,
                         GVariant     *expected_auxiliary_payload)
{
  guint32 user_id;
  GVariant *actual_event_id;
  gint64 relative_time;
  GVariant *actual_auxiliary_payload;
  gboolean singulars_remain =
    g_variant_iter_next (singular_iterator, "(u@ayxmv)", &user_id,
                         &actual_event_id, &relative_time,
                         &actual_auxiliary_payload);

  g_assert_true (singulars_remain);
  g_assert_cmpuint (user_id, ==, USER_ID);

  GVariant *expected_event_id = make_event_id_variant ();
  g_assert_true (g_variant_equal (actual_event_id, expected_event_id));
  g_clear_pointer (&actual_event_id, g_variant_unref);
  g_clear_pointer (&expected_event_id, g_variant_unref);

  g_assert_cmpint (relative_time, ==, OFFSET_TIMESTAMP);

  assert_variants_equal (actual_auxiliary_payload, expected_auxiliary_payload);
  g_clear_pointer (&actual_auxiliary_payload, g_variant_unref);
  g_clear_pointer (&expected_auxiliary_payload, g_variant_unref);
}

static void
assert_aggregate_matches (GVariantIter *aggregate_iterator,
                          GVariant     *expected_auxiliary_payload)
{
  guint32 user_id;
  GVariant *actual_event_id;
  gint64 num_events;
  gint64 relative_time;
  GVariant *actual_auxiliary_payload;
  gboolean aggregates_remain =
    g_variant_iter_next (aggregate_iterator, "(u@ayxxmv)", &user_id,
                         &actual_event_id, &num_events, &relative_time,
                         &actual_auxiliary_payload);

  g_assert_true (aggregates_remain);
  g_assert_cmpuint (user_id, ==, USER_ID);

  GVariant *expected_event_id = make_event_id_variant ();
  g_assert_true (g_variant_equal (actual_event_id, expected_event_id));
  g_variant_unref (actual_event_id);
  g_variant_unref (expected_event_id);

  g_assert_cmpint (num_events, ==, NUM_EVENTS);
  g_assert_cmpint (relative_time, ==, OFFSET_TIMESTAMP);

  assert_variants_equal (actual_auxiliary_payload, expected_auxiliary_payload);
  g_clear_pointer (&actual_auxiliary_payload, g_variant_unref);
  g_clear_pointer (&expected_auxiliary_payload, g_variant_unref);
}

static void
assert_event_value_matches (GVariantIter *event_value_iterator,
                            GVariant     *expected_auxiliary_payload)
{
  gint64 relative_time;
  GVariant *actual_auxiliary_payload;
  gboolean event_values_remain =
    g_variant_iter_next (event_value_iterator, "(xmv)", &relative_time,
                         &actual_auxiliary_payload);
  g_assert_true (event_values_remain);

  g_assert_cmpint (relative_time, ==, OFFSET_TIMESTAMP);

  assert_variants_equal (actual_auxiliary_payload, expected_auxiliary_payload);
  g_clear_pointer (&actual_auxiliary_payload, g_variant_unref);
  g_clear_pointer (&expected_auxiliary_payload, g_variant_unref);
}

static void
assert_sequence_matches (GVariantIter *sequence_iterator)
{
  guint32 user_id;
  GVariant *actual_event_id;
  GVariantIter *event_values_iterator;
  gboolean sequences_remain =
    g_variant_iter_next (sequence_iterator, "(u@aya(xmv))", &user_id,
                         &actual_event_id, &event_values_iterator);

  g_assert_true (sequences_remain);
  g_assert_cmpuint (user_id, ==, USER_ID);

  GVariant *expected_event_id = make_event_id_variant ();
  g_assert_true (g_variant_equal (actual_event_id, expected_event_id));
  g_variant_unref (actual_event_id);
  g_variant_unref (expected_event_id);

  gsize num_event_values = g_variant_iter_n_children (event_values_iterator);
  g_assert_cmpuint (num_event_values, ==, 2u);
  assert_event_value_matches (event_values_iterator,
                              NULL /* auxiliary_payload */);
  assert_event_value_matches (event_values_iterator,
                              g_variant_new_boolean (TRUE));
  g_variant_iter_free (event_values_iterator);
}

static void
record_singulars (EmerDaemon *daemon)
{
  emer_daemon_record_singular_event (daemon,
                                     USER_ID,
                                     make_event_id_variant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     g_variant_new_string ("This must be ignored."));
  emer_daemon_record_singular_event (daemon,
                                     USER_ID,
                                     make_event_id_variant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     g_variant_new_string ("This must be ignored."));
  emer_daemon_record_singular_event (daemon,
                                     USER_ID,
                                     make_event_id_variant (),
                                     RELATIVE_TIMESTAMP,
                                     TRUE,
                                     make_auxiliary_payload ());
}

static void
record_aggregates (EmerDaemon *daemon)
{
  emer_daemon_record_aggregate_event (daemon,
                                      USER_ID,
                                      make_event_id_variant (),
                                      NUM_EVENTS,
                                      RELATIVE_TIMESTAMP,
                                      FALSE,
                                      g_variant_new_string ("This must be ignored."));
  emer_daemon_record_aggregate_event (daemon,
                                      USER_ID,
                                      make_event_id_variant (),
                                      NUM_EVENTS,
                                      RELATIVE_TIMESTAMP,
                                      TRUE,
                                      make_auxiliary_payload ());
}

static void
record_sequence (EmerDaemon *daemon)
{
  emer_daemon_record_event_sequence (daemon,
                                     USER_ID,
                                     make_event_id_variant (),
                                     make_event_values_variant ());
}

static void
get_events_from_request (GByteArray    *request,
                         Fixture       *fixture,
                         GVariantIter **singular_iterator,
                         GVariantIter **aggregate_iterator,
                         GVariantIter **sequence_iterator)
{
  gint64 curr_relative_time;
  gboolean get_succeeded =
    emtr_util_get_current_time (CLOCK_BOOTTIME, &curr_relative_time);
  g_assert_true (get_succeeded);

  gint64 curr_absolute_time;
  get_succeeded =
    emtr_util_get_current_time (CLOCK_REALTIME, &curr_absolute_time);
  g_assert_true (get_succeeded);

  GBytes *request_bytes = g_bytes_new (request->data, request->len);

  gchar *checksum =
    g_compute_checksum_for_bytes (G_CHECKSUM_SHA512, request_bytes);
  gchar *expected_request_path = g_strconcat ("/2/", checksum, NULL);
  g_free (checksum);
  g_assert_cmpstr (fixture->request_path, ==, expected_request_path);
  g_free (expected_request_path);

  const GVariantType *REQUEST_FORMAT =
    G_VARIANT_TYPE ("(ixxaya(uayxmv)a(uayxxmv)a(uaya(xmv)))");
  GVariant *request_variant =
    g_variant_new_from_bytes (REQUEST_FORMAT, request_bytes, FALSE);

  g_bytes_unref (request_bytes);

  g_assert_true (g_variant_is_normal_form (request_variant));

  g_variant_ref_sink (request_variant);
  GVariant *native_endian_request =
    swap_bytes_if_big_endian (request_variant);
  g_variant_unref (request_variant);

  gint32 actual_network_send_number;
  gint64 client_relative_time, client_absolute_time;
  GVariant *machine_id;
  g_variant_get (native_endian_request,
                 "(ixx@aya(uayxmv)a(uayxxmv)a(uaya(xmv)))",
                 &actual_network_send_number, &client_relative_time,
                 &client_absolute_time, &machine_id, singular_iterator,
                 aggregate_iterator, sequence_iterator);

  gint curr_network_send_number =
    emer_network_send_provider_get_send_number (fixture->mock_network_send_provider);
  gint expected_network_send_number = curr_network_send_number - 1;
  g_assert_cmpint (actual_network_send_number, ==,
                   expected_network_send_number);

  g_assert_cmpint (client_relative_time, >=, fixture->relative_time);
  g_assert_cmpint (client_relative_time, <=, curr_relative_time);

  g_assert_cmpint (client_absolute_time, >=, fixture->absolute_time);
  g_assert_cmpint (client_absolute_time, <=, curr_absolute_time);

  assert_machine_id_matches (machine_id, fixture->mock_machine_id_provider);
  g_variant_unref (machine_id);

  g_variant_unref (native_endian_request);
}

static void
assert_no_events_received (GByteArray *request,
                           Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator, *sequence_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator, &sequence_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 0u);
  g_variant_iter_free (singular_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 0u);
  g_variant_iter_free (aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (sequence_iterator), ==, 0u);
  g_variant_iter_free (sequence_iterator);
}

static void
assert_singulars_received (GByteArray *request,
                           Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator, *sequence_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator, &sequence_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 3u);
  assert_singular_matches (singular_iterator, NULL /* auxiliary_payload */);
  assert_singular_matches (singular_iterator, NULL /* auxiliary_payload */);
  assert_singular_matches (singular_iterator, make_auxiliary_payload ());
  g_variant_iter_free (singular_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 0u);
  g_variant_iter_free (aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (sequence_iterator), ==, 0u);
  g_variant_iter_free (sequence_iterator);
}

static void
assert_aggregates_received (GByteArray *request,
                            Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator, *sequence_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator, &sequence_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 0u);
  g_variant_iter_free (singular_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 2u);
  assert_aggregate_matches (aggregate_iterator, NULL /* auxiliary_payload */);
  assert_aggregate_matches (aggregate_iterator, make_auxiliary_payload ());
  g_variant_iter_free (aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (sequence_iterator), ==, 0u);
  g_variant_iter_free (sequence_iterator);
}

static void
assert_sequence_received (GByteArray *request,
                          Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator, *sequence_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator, &sequence_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 0u);
  g_variant_iter_free (singular_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 0u);
  g_variant_iter_free (aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (sequence_iterator), ==, 1u);
  assert_sequence_matches (sequence_iterator);
  g_variant_iter_free (sequence_iterator);
}

static void
handle_upload_finished (EmerDaemon *test_object,
                        GMainLoop  *main_loop)
{
  g_main_loop_quit (main_loop);
}

/* Reads a network request from stdout of fixture->mock_server. Assumes the
 * server prints the path to which the request was made on a single line,
 * followed by the length in bytes of the request received on a single line,
 * followed by the request body without a terminal newline. Sets
 * fixture->relative_time and fixture->absolute_time to the relative and
 * absolute times at which this function was called. Sets fixture->request_path
 * to the path to which the request was sent. Calls source_func with a
 * GByteArray containing the request body as the first parameter and the
 * modified fixture as the second parameter.
 */
static void
read_network_request (Fixture               *fixture,
                      ProcessBytesSourceFunc source_func)
{
  gboolean get_succeeded =
    emtr_util_get_current_time (CLOCK_BOOTTIME, &fixture->relative_time);
  g_assert_true (get_succeeded);
  get_succeeded =
    emtr_util_get_current_time (CLOCK_REALTIME, &fixture->absolute_time);
  g_assert_true (get_succeeded);

  read_lines_from_stdout (fixture->mock_server,
                          (ProcessLineSourceFunc) remove_last_character,
                          &fixture->request_path);

  guint content_length;
  read_lines_from_stdout (fixture->mock_server,
                          (ProcessLineSourceFunc) read_content_length,
                          &content_length);

  read_bytes_from_stdout (fixture->mock_server, content_length, source_func,
                          fixture);
  g_free (fixture->request_path);
}

/* Writes the given HTTP status code to stdin of the given server as a
 * newline-terminated base-10 string. Assumes the server will respond to the
 * client with that code.
 */
static void
send_http_response (GSubprocess *server,
                    gint         status_code)
{
  GOutputStream *output_stream = g_subprocess_get_stdin_pipe (server);
  gchar *status_string = g_strdup_printf ("%d\n", status_code);
  gsize string_length = strlen (status_string);
  gboolean write_succeeded =
    g_output_stream_write_all (output_stream, status_string, string_length,
                               NULL /* bytes written */,
                               NULL /* GCancellable */, NULL /* GError */);
  g_assert_true (write_succeeded);
  g_free (status_string);

  gboolean flush_succeeded =
    g_output_stream_flush (output_stream, NULL /* GCancellable */,
                           NULL /* GError */);
  g_assert_true (flush_succeeded);
}

static void
wait_for_upload_to_finish (Fixture *fixture)
{
  GMainLoop *main_loop = g_main_loop_new (NULL /* GMainContext */, FALSE);
  guint handler_id =
    g_signal_connect (fixture->test_object, "upload-finished",
                      G_CALLBACK (handle_upload_finished), main_loop);

  send_http_response (fixture->mock_server, SOUP_STATUS_OK);

  guint timeout_id =
    g_timeout_add_seconds (TIMEOUT_SEC, timeout, NULL /* user data */);

  g_main_loop_run (main_loop);

  g_source_remove (timeout_id);
  g_signal_handler_disconnect (fixture->test_object, handler_id);
  g_main_loop_unref (main_loop);
}

static gchar *
get_server_uri (GSubprocess *mock_server)
{
  gchar *port_number;
  read_lines_from_stdout (mock_server,
                          (ProcessLineSourceFunc) remove_last_character,
                          &port_number);
  gchar *server_uri = g_strconcat ("http://localhost:", port_number, "/", NULL);
  g_free (port_number);
  return server_uri;
}

// Setup/Teardown functions next:

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  fixture->mock_server =
    g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_PIPE |
                        G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                      NULL, MOCK_SERVER_PATH, NULL);
  g_assert_nonnull (fixture->mock_server);

  gchar *server_uri = get_server_uri (fixture->mock_server);

  fixture->mock_machine_id_provider = emer_machine_id_provider_new ();
  fixture->mock_network_send_provider = emer_network_send_provider_new ();
  fixture->mock_permissions_provider = emer_permissions_provider_new ();
  fixture->mock_persistent_cache = emer_persistent_cache_new (NULL, NULL);
  fixture->test_object =
    emer_daemon_new_full (g_rand_new_with_seed (18),
                          server_uri,
                          2, // Network Send Interval
                          fixture->mock_machine_id_provider,
                          fixture->mock_network_send_provider,
                          fixture->mock_permissions_provider,
                          fixture->mock_persistent_cache,
                          20); // Buffer length
  g_free (server_uri);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_object_unref (fixture->test_object);
  g_object_unref (fixture->mock_machine_id_provider);
  g_object_unref (fixture->mock_network_send_provider);
  g_object_unref (fixture->mock_permissions_provider);
  g_object_unref (fixture->mock_persistent_cache);
  terminate_subprocess_and_wait (fixture->mock_server);
}

// Unit Tests next:

static void
test_daemon_new_succeeds (Fixture      *fixture,
                          gconstpointer unused)
{
  EmerDaemon *daemon = emer_daemon_new ();
  g_assert_nonnull (daemon);
  g_object_unref (daemon);
}

static void
test_daemon_new_full_succeeds (Fixture      *fixture,
                               gconstpointer unused)
{
  g_assert_nonnull (fixture->test_object);
}

static void
test_daemon_records_singulars (Fixture      *fixture,
                               gconstpointer unused)
{
  record_singulars (fixture->test_object);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_records_aggregates (Fixture      *fixture,
                                gconstpointer unused)
{
  record_aggregates (fixture->test_object);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_records_sequence (Fixture      *fixture,
                              gconstpointer unused)
{
  record_sequence (fixture->test_object);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_sequence_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_retries_singular_uploads (Fixture      *fixture,
                                      gconstpointer unused)
{
  record_singulars (fixture->test_object);

  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);
  send_http_response (fixture->mock_server, SOUP_STATUS_INTERNAL_SERVER_ERROR);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Attempt to upload metrics failed: Internal Server "
                         "Error.");
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);
  wait_for_upload_to_finish (fixture);
  g_test_assert_expected_messages ();
}

static void
test_daemon_retries_aggregate_uploads (Fixture      *fixture,
                                       gconstpointer unused)
{
  record_aggregates (fixture->test_object);

  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  send_http_response (fixture->mock_server, SOUP_STATUS_INTERNAL_SERVER_ERROR);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Attempt to upload metrics failed: Internal Server "
                         "Error.");
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  wait_for_upload_to_finish (fixture);
  g_test_assert_expected_messages ();
}

static void
test_daemon_retries_sequence_uploads (Fixture      *fixture,
                                      gconstpointer unused)
{
  record_sequence (fixture->test_object);

  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_sequence_received);
  send_http_response (fixture->mock_server, SOUP_STATUS_INTERNAL_SERVER_ERROR);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Attempt to upload metrics failed: Internal Server "
                         "Error.");
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_sequence_received);
  wait_for_upload_to_finish (fixture);
  g_test_assert_expected_messages ();
}

static void
test_daemon_only_reports_singulars_when_uploading_enabled (Fixture      *fixture,
                                                           gconstpointer unused)
{
  mock_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   FALSE);
  record_singulars (fixture->test_object);
  assert_uploading_disabled (fixture);

  mock_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_only_reports_aggregates_when_uploading_enabled (Fixture      *fixture,
                                                            gconstpointer unused)
{
  mock_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   FALSE);
  record_aggregates (fixture->test_object);
  assert_uploading_disabled (fixture);

  mock_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_only_reports_sequences_when_uploading_enabled (Fixture      *fixture,
                                                           gconstpointer unused)
{
  mock_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   FALSE);
  record_sequence (fixture->test_object);
  assert_uploading_disabled (fixture);

  mock_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_sequence_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_does_not_record_singulars_when_daemon_disabled (Fixture      *fixture,
                                                            gconstpointer unused)
{
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  record_singulars (fixture->test_object);
  assert_uploading_disabled (fixture);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_no_events_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_does_not_record_aggregates_when_daemon_disabled (Fixture      *fixture,
                                                             gconstpointer unused)
{
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  record_aggregates (fixture->test_object);
  assert_uploading_disabled (fixture);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_no_events_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_does_not_record_sequences_when_daemon_disabled (Fixture      *fixture,
                                                            gconstpointer unused)
{
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  record_sequence (fixture->test_object);
  assert_uploading_disabled (fixture);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_no_events_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_inhibits_shutdown (Fixture      *fixture,
                               gconstpointer unused)
{
  start_mock_logind_service (fixture);
  read_lines_from_stdout (fixture->logind_mock,
                          (ProcessLineSourceFunc) process_logind_line,
                          NULL /* user_data */);
  terminate_subprocess_and_wait (fixture->logind_mock);
}

static void
test_daemon_updates_timestamps_on_shutdown (Fixture      *fixture,
                                            gconstpointer unused)
{
  start_mock_logind_service (fixture);

  gint num_timestamp_updates_before =
    mock_persistent_cache_get_num_timestamp_updates (fixture->mock_persistent_cache);

  read_lines_from_stdout (fixture->logind_mock,
                          (ProcessLineSourceFunc) process_logind_line,
                          NULL /* user_data */);
  emit_shutdown_signal (TRUE);

  // Wait for EmerDaemon to handle the signal.
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, TRUE);

  gint num_timestamp_updates_after =
    mock_persistent_cache_get_num_timestamp_updates (fixture->mock_persistent_cache);
  g_assert_cmpint (num_timestamp_updates_after, ==, num_timestamp_updates_before + 1);

  terminate_subprocess_and_wait (fixture->logind_mock);
}

static void
test_daemon_flushes_to_persistent_cache_on_shutdown (Fixture      *fixture,
                                                     gconstpointer unused)
{
  start_mock_logind_service (fixture);

  gint num_calls_before =
    mock_persistent_cache_get_store_metrics_called (fixture->mock_persistent_cache);

  read_lines_from_stdout (fixture->logind_mock,
                          (ProcessLineSourceFunc) process_logind_line,
                          NULL /* user_data */);
  emit_shutdown_signal (TRUE);

  // Wait for EmerDaemon to handle the signal.
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, TRUE);

  gint num_calls_after =
    mock_persistent_cache_get_store_metrics_called (fixture->mock_persistent_cache);
  g_assert_cmpint (num_calls_after, ==, num_calls_before + 1);

  terminate_subprocess_and_wait (fixture->logind_mock);
}

static void
test_daemon_reinhibits_shutdown_on_shutdown_cancel (Fixture      *fixture,
                                                    gconstpointer unused)
{
  start_mock_logind_service (fixture);

  read_lines_from_stdout (fixture->logind_mock,
                          (ProcessLineSourceFunc) process_logind_line,
                          NULL /* user_data */);
  emit_shutdown_signal (TRUE);

  // Wait for EmerDaemon to handle the signal.
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, TRUE);

  emit_shutdown_signal (FALSE);
  read_lines_from_stdout (fixture->logind_mock,
                          (ProcessLineSourceFunc) process_logind_line,
                          NULL /* user_data */);

  terminate_subprocess_and_wait (fixture->logind_mock);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

#define ADD_DAEMON_TEST(path, test_func) \
  g_test_add ((path), Fixture, NULL, setup, (test_func), teardown)

  ADD_DAEMON_TEST ("/daemon/new-succeeds", test_daemon_new_succeeds);
  ADD_DAEMON_TEST ("/daemon/new-full-succeeds", test_daemon_new_full_succeeds);
  ADD_DAEMON_TEST ("/daemon/records-singulars", test_daemon_records_singulars);
  ADD_DAEMON_TEST ("/daemon/records-aggregates",
                   test_daemon_records_aggregates);
  ADD_DAEMON_TEST ("/daemon/records-sequence", test_daemon_records_sequence);
  ADD_DAEMON_TEST ("/daemon/retries-singular-uploads",
                   test_daemon_retries_singular_uploads);
  ADD_DAEMON_TEST ("/daemon/retries-aggregate-uploads",
                   test_daemon_retries_aggregate_uploads);
  ADD_DAEMON_TEST ("/daemon/retries-sequence-uploads",
                   test_daemon_retries_sequence_uploads);
  ADD_DAEMON_TEST ("/daemon/only-reports-singulars-when-uploading-enabled",
                   test_daemon_only_reports_singulars_when_uploading_enabled);
  ADD_DAEMON_TEST ("/daemon/only-reports-aggregates-when-uploading-enabled",
                   test_daemon_only_reports_aggregates_when_uploading_enabled);
  ADD_DAEMON_TEST ("/daemon/only-reports-sequences-when-uploading-enabled",
                   test_daemon_only_reports_sequences_when_uploading_enabled);
  ADD_DAEMON_TEST ("/daemon/does-not-record-singulars-when-daemon-disabled",
                   test_daemon_does_not_record_singulars_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/does-not-record-aggregates-when-daemon-disabled",
                   test_daemon_does_not_record_aggregates_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/does-not-record-sequences-when-daemon-disabled",
                   test_daemon_does_not_record_sequences_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/inhibits-shutdown", test_daemon_inhibits_shutdown);
  ADD_DAEMON_TEST ("/daemon/updates-timestamps-on-shutdown",
                   test_daemon_updates_timestamps_on_shutdown);
  ADD_DAEMON_TEST ("/daemon/flushes-to-persistent-cache-on-shutdown",
                   test_daemon_flushes_to_persistent_cache_on_shutdown);
  ADD_DAEMON_TEST ("/daemon/reinhibits-shutdown-on-shutdown-cancel",
                   test_daemon_reinhibits_shutdown_on_shutdown_cancel);

#undef ADD_DAEMON_TEST

  return g_test_run ();
}
