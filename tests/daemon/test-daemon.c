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

#include "config.h"
#include "emer-daemon.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <uuid/uuid.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include <eosmetrics/eosmetrics.h>

#include "emer-boot-id-provider.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"
#include "emer-types.h"
#include "mock-image-id-provider.h"
#include "mock-permissions-provider.h"
#include "mock-persistent-cache.h"
#include "shared/metrics-util.h"

#define MOCK_SERVER_PATH TEST_DIR "daemon/mock-server.py"

#define MEANINGLESS_EVENT "350ac4ff-3026-4c25-9e7e-e8103b4fd5d8"

#define NUM_EVENTS 101u
#define RELATIVE_TIMESTAMP G_GINT64_CONSTANT (123456789)
#define OFFSET_TIMESTAMP (RELATIVE_TIMESTAMP + BOOT_TIME_OFFSET)

#define MAX_REQUEST_PAYLOAD 100000

/* The non-array portion of the singular in make_large_singular costs 60 bytes,
 * including some for the OS Version string.
 */
#define ZERO_ARRAY_LENGTH (MAX_REQUEST_PAYLOAD - 44)

#define TIMEOUT_SEC 5

typedef struct _Fixture
{
  EmerDaemon *test_object;
  EmerPermissionsProvider *mock_permissions_provider;
  EmerPersistentCache *mock_persistent_cache;
  EmerAggregateTally *mock_aggregate_tally;

  GSubprocess *mock_server;
  gchar *server_uri;

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
  EmerError error_code;
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
  g_autoptr(GError) error = NULL;
  g_assert_false (g_subprocess_wait_check (subprocess, NULL, &error));
  g_assert_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "Child process killed by signal 15");

  g_object_unref (subprocess);
}

static gboolean
timeout (gpointer unused)
{
  g_assert_not_reached ();
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
  if (num_bytes_read < 0)
    {
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
      g_error_free (error);
      return FALSE;
    }

  guint new_length = byte_array->len + num_bytes_read;
  g_byte_array_set_size (byte_array, new_length);
  return (gsize) num_bytes_read == num_bytes_remaining;
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

  g_source_set_callback (stdout_source, G_SOURCE_FUNC (collect_lines),
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

  g_source_set_callback (stdout_source, G_SOURCE_FUNC (collect_bytes),
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

static GVariant *
make_variant_for_event_id (const gchar *event_id)
{
  uuid_t uuid;
  g_assert_cmpint (uuid_parse (event_id, uuid), ==, 0);
  return get_uuid_as_variant (uuid);
}

static GVariant *
make_event_id_variant (void)
{
  return make_variant_for_event_id (MEANINGLESS_EVENT);
}

static GVariant *
make_auxiliary_payload (void)
{
  GVariant *sword_of_a_thousand = g_variant_new_boolean (TRUE);
  return g_variant_new_variant (sword_of_a_thousand);
}

static GVariant *
make_large_singular (void)
{
  static guchar array[ZERO_ARRAY_LENGTH];
  GVariant *auxiliary_payload =
    g_variant_new_fixed_array (G_VARIANT_TYPE ("y"), array, ZERO_ARRAY_LENGTH,
                               1);
  GVariant *singular =
    g_variant_new ("(@aysxmv)", make_event_id_variant (),
                   emer_image_id_provider_get_os_version(),
                   OFFSET_TIMESTAMP, auxiliary_payload);

  gsize singular_cost = emer_persistent_cache_cost (singular);
  g_assert_cmpuint (singular_cost, ==, MAX_REQUEST_PAYLOAD);

  return singular;
}

static void
assert_no_data_uploaded (EmerDaemon               *daemon,
                         GAsyncResult             *result,
                         UploadEventsCallbackData *callback_data)
{
  GError *error = NULL;
  g_assert_false (emer_daemon_upload_events_finish (daemon, result, &error));
  g_assert_error (error, EMER_ERROR, (gint) callback_data->error_code);
  g_error_free (error);

  GPollableInputStream *pollable_input_stream =
    get_pollable_input_stream (callback_data->mock_server);
  g_assert_false (read_byte (pollable_input_stream, NULL /* byte */));

  g_main_loop_quit (callback_data->main_loop);
}

static void
assert_upload_events_fails (Fixture     *fixture,
                            EmerError    error_code)
{
  UploadEventsCallbackData callback_data =
    {
      g_main_loop_new (NULL /* GMainContext */, FALSE),
      fixture->mock_server,
      error_code,
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
assert_uploading_disabled (Fixture     *fixture)
{
  assert_upload_events_fails (fixture, EMER_ERROR_UPLOADING_DISABLED);
}

static void
assert_metrics_disabled (Fixture     *fixture)
{
  assert_upload_events_fails (fixture, EMER_ERROR_METRICS_DISABLED);
}

static void
assert_variants_equal (GVariant *actual_variant,
                       GVariant *expected_variant)
{
  g_assert_nonnull (actual_variant);
  g_assert_nonnull (expected_variant);

  g_assert_cmpvariant (actual_variant, expected_variant);

  g_variant_unref (actual_variant);
  g_variant_unref (expected_variant);
}

static void
assert_singular_matches_variant (GVariant *actual_variant,
                                 GVariant *expected_auxiliary_payload)
{
  GVariant *expected_variant =
    g_variant_new ("(@aysxm@v)", make_event_id_variant (),
                   emer_image_id_provider_get_os_version(),
                   OFFSET_TIMESTAMP, expected_auxiliary_payload);
  assert_variants_equal (actual_variant, expected_variant);
}

static void
assert_aggregate_matches_variant (GVariant   *actual_variant,
                                  const char *expected_period_start,
                                  GVariant   *expected_auxiliary_payload)
{
  GVariant *expected_variant =
    g_variant_new ("(@ayssum@v)", make_event_id_variant (),
                   emer_image_id_provider_get_os_version(),
                   expected_period_start,
                   NUM_EVENTS, expected_auxiliary_payload);
  assert_variants_equal (actual_variant, expected_variant);
}

static void
assert_singular_matches_next_value (GVariantIter *singular_iterator,
                                    GVariant     *expected_auxiliary_payload)
{
  GVariant *singular = g_variant_iter_next_value (singular_iterator);
  assert_singular_matches_variant (singular, expected_auxiliary_payload);
}

static void
assert_aggregate_matches_next_value (GVariantIter *aggregate_iterator,
                                     const gchar  *expected_period_start,
                                     GVariant     *expected_auxiliary_payload)
{
  GVariant *aggregate = g_variant_iter_next_value (aggregate_iterator);
  assert_aggregate_matches_variant (aggregate, expected_period_start, expected_auxiliary_payload);
}

static void
assert_singulars_match (GVariant **variants,
                        gsize      num_variants)
{
  g_assert_cmpuint (num_variants, ==, 3);
  assert_singular_matches_variant (variants[0], NULL /* auxiliary payload */);
  assert_singular_matches_variant (variants[1], NULL /* auxiliary payload */);
  assert_singular_matches_variant (variants[2], make_auxiliary_payload ());
}

static void
record_singulars (EmerDaemon *daemon)
{
  emer_daemon_record_singular_event (daemon,
                                     make_event_id_variant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     g_variant_new_variant (g_variant_new_string ("This must be ignored.")));
  GVariant *auxiliary_payload = g_variant_new_variant (g_variant_new_boolean (FALSE));
  g_variant_ref_sink (auxiliary_payload);
  emer_daemon_record_singular_event (daemon,
                                     make_event_id_variant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     auxiliary_payload);
  g_variant_unref (auxiliary_payload);
  emer_daemon_record_singular_event (daemon,
                                     make_event_id_variant (),
                                     RELATIVE_TIMESTAMP,
                                     TRUE,
                                     make_auxiliary_payload ());
}

static void
record_aggregates (EmerDaemon *daemon)
{
  emer_daemon_enqueue_aggregate_event (daemon,
                                       make_event_id_variant (),
                                       "2021-08-27",
                                       NUM_EVENTS,
                                       NULL);
  emer_daemon_enqueue_aggregate_event (daemon,
                                       make_event_id_variant (),
                                       "2021-08",
                                       NUM_EVENTS,
                                       make_auxiliary_payload ());
}

static void
get_events_from_request (GByteArray    *request,
                         Fixture       *fixture,
                         GVariantIter **singular_iterator,
                         GVariantIter **aggregate_iterator)
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
  gchar *expected_request_path = g_build_filename ("/3/", checksum, NULL);
  g_free (checksum);
  g_assert_cmpstr (fixture->request_path, ==, expected_request_path);
  g_free (expected_request_path);

  const GVariantType *REQUEST_FORMAT =
    G_VARIANT_TYPE ("(xxsa{ss}ya(aysxmv)a(ayssumv))");
  GVariant *request_variant =
    g_variant_new_from_bytes (REQUEST_FORMAT, request_bytes, FALSE);

  g_bytes_unref (request_bytes);

  g_assert_true (g_variant_is_normal_form (request_variant));

  g_variant_ref_sink (request_variant);
  GVariant *native_endian_request =
    swap_bytes_if_big_endian (request_variant);
  g_variant_unref (request_variant);

  gint64 client_relative_time, client_absolute_time;
  const gchar *image_version;
  GVariant *site_id;
  guint8 boot_type;
  g_variant_get (native_endian_request,
                 "(xx&s@a{ss}ya(aysxmv)a(ayssumv))",
                 &client_relative_time, &client_absolute_time, &image_version,
                 &site_id, &boot_type, singular_iterator, aggregate_iterator);

  g_assert_cmpint (client_relative_time, >=, fixture->relative_time);
  g_assert_cmpint (client_relative_time, <=, curr_relative_time);

  g_assert_cmpint (client_absolute_time, >=, fixture->absolute_time);
  g_assert_cmpint (client_absolute_time, <=, curr_absolute_time);

  g_assert_cmpstr (image_version, ==, IMAGE_VERSION);

  const gchar *site_value;
  g_assert_cmpint (g_variant_n_children (site_id), ==, 2);
  g_variant_lookup (site_id, "id", "&s", &site_value);
  g_assert_cmpstr (site_value, ==, "myid");
  g_variant_lookup (site_id, "country", "&s", &site_value);
  g_assert_cmpstr (site_value, ==, "Earth");
  g_variant_unref (site_id);

  g_variant_unref (native_endian_request);
}

static void
assert_no_events_received (GByteArray *request,
                           Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 0u);
  g_variant_iter_free (singular_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 0u);
  g_variant_iter_free (aggregate_iterator);
}

static void
assert_singulars_received (GByteArray *request,
                           Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 3u);
  assert_singular_matches_next_value (singular_iterator,
                                      NULL /* auxiliary_payload */);
  assert_singular_matches_next_value (singular_iterator,
                                      NULL /* auxiliary_payload */);
  assert_singular_matches_next_value (singular_iterator,
                                      make_auxiliary_payload ());
  g_variant_iter_free (singular_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 0u);
  g_variant_iter_free (aggregate_iterator);
}

static void
assert_large_singular_received (GByteArray *request,
                                Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 1u);
  GVariant *actual_singular = g_variant_iter_next_value (singular_iterator);
  g_variant_iter_free (singular_iterator);
  assert_variants_equal (actual_singular, make_large_singular ());

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 0u);
  g_variant_iter_free (aggregate_iterator);
}

static void
assert_aggregates_received (GByteArray *request,
                            Fixture    *fixture)
{
  GVariantIter *singular_iterator, *aggregate_iterator;
  get_events_from_request (request, fixture, &singular_iterator,
                           &aggregate_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (singular_iterator), ==, 0u);
  g_variant_iter_free (singular_iterator);

  g_assert_cmpuint (g_variant_iter_n_children (aggregate_iterator), ==, 2u);
  assert_aggregate_matches_next_value (aggregate_iterator,
                                       "2021-08-27",
                                       NULL /* auxiliary_payload */);
  assert_aggregate_matches_next_value (aggregate_iterator,
                                       "2021-08",
                                       make_auxiliary_payload ());
  g_variant_iter_free (aggregate_iterator);
}

static void
handle_upload_finished (EmerDaemon *test_object,
                        GMainLoop  *main_loop)
{
  g_main_loop_quit (main_loop);
}

/* Reads a network request from stdout of fixture->mock_server. Assumes the
 * server prints the path to which the request was made on a single line,
 * followed by the content encoding on a single line, followed by the length in
 * bytes of the request received on a single line, followed by the request body
 * without a terminal newline. Sets fixture->relative_time and
 * fixture->absolute_time to the relative and absolute times at which this
 * function was called. Sets fixture->request_path to the path to which the
 * request was sent. Calls source_func with a GByteArray containing the request
 * body as the first parameter and the modified fixture as the second parameter.
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

  gchar *content_encoding;
  read_lines_from_stdout (fixture->mock_server,
                          (ProcessLineSourceFunc) remove_last_character,
                          &content_encoding);
  g_assert_cmpstr (content_encoding, ==, "gzip");

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

static void
reap_when_parent_dies (gpointer unused)
{
  g_assert_cmpint (prctl (PR_SET_PDEATHSIG, SIGTERM), ==, 0);
}

// Setup/Teardown functions next:

static void
create_test_object (Fixture *fixture)
{
  fixture->test_object =
    emer_daemon_new_full (g_rand_new_with_seed (18),
                          fixture->server_uri,
                          2 /* network send interval */,
                          fixture->mock_permissions_provider,
                          fixture->mock_persistent_cache,
                          fixture->mock_aggregate_tally,
                          100000 /* max bytes buffered */);
}

static void
setup_most (Fixture      *fixture,
            gconstpointer unused)
{
  // The mock server should be sent SIGTERM when this process exits.
  GSubprocessLauncher *subprocess_launcher =
    g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDIN_PIPE |
                               G_SUBPROCESS_FLAGS_STDOUT_PIPE);
  g_subprocess_launcher_set_child_setup (subprocess_launcher,
                                         reap_when_parent_dies,
                                         NULL /* user data */,
                                         NULL /* GDestroyNotify */);
  fixture->mock_server =
    g_subprocess_launcher_spawn (subprocess_launcher, NULL, MOCK_SERVER_PATH,
                                 NULL);
  g_assert_nonnull (fixture->mock_server);
  g_object_unref (subprocess_launcher);

  fixture->server_uri = get_server_uri (fixture->mock_server);

  fixture->mock_permissions_provider = emer_permissions_provider_new ();
  fixture->mock_persistent_cache = NULL;
  /* Not actually a mock! */
  fixture->mock_aggregate_tally = emer_aggregate_tally_new (g_get_user_cache_dir ());
}

static void
setup_persistent_cache (Fixture *fixture)
{
  g_autoptr(GError) error = NULL;

  /* directory and max_cache_size are ignored by mock object. */
  fixture->mock_persistent_cache =
    emer_persistent_cache_new (NULL /* directory */,
                               10000000, /* max_cache_size */
                               FALSE /* reinitialize_cache */,
                               &error);
  g_assert_no_error (error);
}

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  setup_most (fixture, unused);
  setup_persistent_cache (fixture);
  create_test_object (fixture);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_clear_object (&fixture->test_object);
  g_clear_object (&fixture->mock_permissions_provider);
  g_clear_object (&fixture->mock_persistent_cache);
  g_clear_object (&fixture->mock_aggregate_tally);
  g_clear_pointer (&fixture->mock_server, terminate_subprocess_and_wait);
  g_clear_pointer (&fixture->server_uri, g_free);
}

// Unit Tests next:

static void
test_daemon_new_succeeds (Fixture      *fixture,
                          gconstpointer unused)
{
  EmerDaemon *daemon = emer_daemon_new (NULL /* persistent cache directory */,
                                        NULL /* permissions provider */);
  g_assert_nonnull (daemon);
  g_object_unref (daemon);
}

static void
test_daemon_new_succeeds_if_disabled (Fixture       *fixture,
                                      gconstpointer  unused)
{
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider, FALSE);

  EmerDaemon *daemon = emer_daemon_new (NULL /* persistent cache directory */,
                                        fixture->mock_permissions_provider);
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
test_daemon_only_reports_singulars_when_uploading_enabled (Fixture      *fixture,
                                                           gconstpointer unused)
{
  emer_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   FALSE);
  record_singulars (fixture->test_object);
  assert_uploading_disabled (fixture);

  emer_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_only_reports_aggregates_when_uploading_enabled (Fixture      *fixture,
                                                            gconstpointer unused)
{
  emer_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   FALSE);
  record_aggregates (fixture->test_object);
  assert_uploading_disabled (fixture);

  emer_permissions_provider_set_uploading_enabled (fixture->mock_permissions_provider,
                                                   TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_does_not_record_singulars_when_daemon_disabled (Fixture      *fixture,
                                                            gconstpointer unused)
{
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  record_singulars (fixture->test_object);
  assert_metrics_disabled (fixture);

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
  assert_metrics_disabled (fixture);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_no_events_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_discards_in_memory_singulars_when_daemon_disabled (Fixture      *fixture,
                                                               gconstpointer unused)
{
  /* Record some events, but disable metrics before they can be submitted */
  record_singulars (fixture->test_object);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  assert_metrics_disabled (fixture);
  g_assert_true (mock_persistent_cache_is_empty (fixture->mock_persistent_cache));

  /* Re-enable, and wait for the next tick. No events should be submitted. */
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_no_events_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_discards_in_flight_singulars_when_daemon_disabled (Fixture      *fixture,
                                                               gconstpointer unused)
{
  /* Record some events, and wait for them to be submitted */
  record_singulars (fixture->test_object);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);

  /* Before the server sends a reply, disable the daemon */
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  assert_metrics_disabled (fixture);
  g_assert_true (mock_persistent_cache_is_empty (fixture->mock_persistent_cache));

  /* Now allow the server to reply */
  wait_for_upload_to_finish (fixture);

  /* Re-enable, and wait for the next tick. No (more) events should be submitted. */
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_no_events_received);
  wait_for_upload_to_finish (fixture);

  /* Record some different events, and ensure they're submitted correctly */
  record_aggregates (fixture->test_object);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_discards_failed_in_flight_singulars_when_daemon_disabled (Fixture      *fixture,
                                                                      gconstpointer unused)
{
  /* Record some events, and wait for them to be submitted */
  record_singulars (fixture->test_object);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);

  /* Before the server sends a reply, disable the daemon */
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  assert_metrics_disabled (fixture);
  g_assert_true (mock_persistent_cache_is_empty (fixture->mock_persistent_cache));

  /* Re-enable the daemon, and send some different events */
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  record_aggregates (fixture->test_object);

  /* Now send back an error to the first batch  */
  send_http_response (fixture->mock_server, SOUP_STATUS_INTERNAL_SERVER_ERROR);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Attempt to upload metrics failed: Internal Server "
                         "Error.");

  /* The first batch, which were pending when the daemon was disabled, should
   * have been discarded; the new batch should be sent.
   */
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  /* By now it should have warned about the first attempt */
  g_test_assert_expected_messages ();
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_discards_persistent_cache_when_daemon_disabled (Fixture      *fixture,
                                                            gconstpointer unused)
{
  /* Pre-seed the persistent cache with an event */
  g_autoptr(GVariant) variant = g_variant_ref_sink (make_large_singular ());
  gsize num_variants_stored;
  gboolean store_succeeded =
    emer_persistent_cache_store (fixture->mock_persistent_cache, &variant, 1,
                                 &num_variants_stored,
                                 NULL /* GError */);
  g_assert_true (store_succeeded);
  g_assert_cmpuint (num_variants_stored, ==, 1);

  /* Disable metrics before that event can be submitted */
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                FALSE);
  assert_metrics_disabled (fixture);
  g_assert_true (mock_persistent_cache_is_empty (fixture->mock_persistent_cache));

  /* Re-enable, and wait for the next tick. No events should be submitted. */
  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_provider,
                                                TRUE);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_no_events_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_flushes_to_persistent_cache_on_finalize (Fixture      *fixture,
                                                     gconstpointer unused)
{
  record_singulars (fixture->test_object);

  /* Unref the daemon, causing it to finalize. */
  g_clear_object (&fixture->test_object);

  GVariant **variants;
  gsize num_variants;
  guint64 token;
  gboolean has_invalid;
  gboolean read_succeeded =
    emer_persistent_cache_read (fixture->mock_persistent_cache, &variants,
                                G_MAXSIZE, &num_variants, &token, &has_invalid,
                                NULL /* GError */);
  g_assert_true (read_succeeded);
  g_assert_false (has_invalid);
  assert_singulars_match (variants, num_variants);

  /* Create a new daemon */
  create_test_object (fixture);

  /* It should upload those cached events */
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_singulars_received);
  wait_for_upload_to_finish (fixture);
}

static void
test_daemon_limits_network_upload_size (Fixture      *fixture,
                                        gconstpointer unused)
{
  GVariant *variant = make_large_singular ();
  g_variant_ref_sink (variant);
  GVariant *variants[] = { variant, variant };
  gsize num_variants = G_N_ELEMENTS (variants);

  gsize num_variants_stored;
  gboolean store_succeeded =
    emer_persistent_cache_store (fixture->mock_persistent_cache, variants,
                                 num_variants, &num_variants_stored,
                                 NULL /* GError */);
  g_variant_unref (variant);

  g_assert_true (store_succeeded);
  g_assert_cmpuint (num_variants_stored, ==, num_variants);

  record_aggregates (fixture->test_object);

  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_large_singular_received);
  wait_for_upload_to_finish (fixture);

  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_large_singular_received);
  wait_for_upload_to_finish (fixture);

  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  wait_for_upload_to_finish (fixture);
}

/* If the first attempt to create the EmerPersistentCache fails with a
 * G_KEY_FILE_ERROR, the daemon should attempt to reset the cache, and log an
 * event indicating that the cache metadata was corrupt.
 */
static void
test_daemon_reinitializes_cache_on_key_file_error (Fixture      *fixture,
                                                   gconstpointer unused)
{
  g_autoptr(GError) persistent_cache_error =
    g_error_new (G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE, "oh no");
  mock_persistent_cache_set_construct_error (persistent_cache_error);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*oh no*");
  create_test_object (fixture);

  g_object_get (fixture->test_object,
                "persistent-cache", &fixture->mock_persistent_cache,
                NULL);

  g_assert_true (mock_persistent_cache_get_reinitialize (fixture->mock_persistent_cache));

  g_test_assert_expected_messages ();
}

/* If creating the EmerPersistentCache with an error other than
 * G_KEY_FILE_ERROR, we expect a crash.
 */
static void
test_daemon_crashes_on_non_key_file_error (Fixture      *fixture,
                                           gconstpointer unused)
{
  if (g_test_subprocess ())
    {
      g_autoptr(GError) persistent_cache_error =
        g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "oh no");

      mock_persistent_cache_set_construct_error (persistent_cache_error);

      /* should abort */
      create_test_object (fixture);
      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_failed ();
  g_test_trap_assert_stderr ("*oh no*");
}

/* Aggregate tally entries from a previous day & month should be
 * submitted when starting up on a new day & month.
 */
static void
test_daemon_submits_aggregates_from_tally_on_startup (Fixture       *fixture,
                                                      gconstpointer  unused)
{
  const guint32 uid = 12345;
  g_autoptr(GDateTime) the_past = g_date_time_new_local (2021, 8, 27, 0, 0, 0);
  g_autoptr(GError) error = NULL;
  uuid_t uuid;
  g_assert_cmpint (uuid_parse (MEANINGLESS_EVENT, uuid), ==, 0);

  emer_aggregate_tally_store_event (fixture->mock_aggregate_tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    uid,
                                    uuid,
                                    NULL,
                                    NUM_EVENTS,
                                    the_past,
                                    &error);
  g_assert_no_error (error);

  emer_aggregate_tally_store_event (fixture->mock_aggregate_tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    uid,
                                    uuid,
                                    make_auxiliary_payload (),
                                    NUM_EVENTS,
                                    the_past,
                                    &error);
  g_assert_no_error (error);

  setup_persistent_cache (fixture);
  create_test_object (fixture);
  read_network_request (fixture,
                        (ProcessBytesSourceFunc) assert_aggregates_received);
  wait_for_upload_to_finish (fixture);
}

/* TODO: as above, but add events to the tally while the daemon is running
 * then simulate the day & month changing.
 */

gint
main (gint                argc,
      const gchar * const argv[])
{
  /* TODO: without this, when the daemon uses GNetworkMonitor, Gio tries to load a module. But it fails to load because G_TEST_OPTION_ISOLATE_DIRS overwrites $XDG_DATA_DIRS, so the module cannot find the schemas installed in /usr.
   */
  g_setenv ("GIO_MODULE_DIR", "/dev/null", TRUE);
  g_test_init (&argc, (gchar ***) &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

#define ADD_DAEMON_TEST(path, test_func) \
  g_test_add ((path), Fixture, NULL, setup, (test_func), teardown)

  ADD_DAEMON_TEST ("/daemon/new-succeeds", test_daemon_new_succeeds);
  ADD_DAEMON_TEST ("/daemon/new-succeeds-if-disabled", test_daemon_new_succeeds_if_disabled);
  ADD_DAEMON_TEST ("/daemon/new-full-succeeds", test_daemon_new_full_succeeds);
  ADD_DAEMON_TEST ("/daemon/records-singulars", test_daemon_records_singulars);
  ADD_DAEMON_TEST ("/daemon/records-aggregates",
                   test_daemon_records_aggregates);
  ADD_DAEMON_TEST ("/daemon/retries-singular-uploads",
                   test_daemon_retries_singular_uploads);
  ADD_DAEMON_TEST ("/daemon/retries-aggregate-uploads",
                   test_daemon_retries_aggregate_uploads);
  ADD_DAEMON_TEST ("/daemon/only-reports-singulars-when-uploading-enabled",
                   test_daemon_only_reports_singulars_when_uploading_enabled);
  ADD_DAEMON_TEST ("/daemon/only-reports-aggregates-when-uploading-enabled",
                   test_daemon_only_reports_aggregates_when_uploading_enabled);
  ADD_DAEMON_TEST ("/daemon/does-not-record-singulars-when-daemon-disabled",
                   test_daemon_does_not_record_singulars_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/does-not-record-aggregates-when-daemon-disabled",
                   test_daemon_does_not_record_aggregates_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/discards/in-memory-singulars-when-daemon-disabled",
                   test_daemon_discards_in_memory_singulars_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/discards/in-flight-singulars-when-daemon-disabled",
                   test_daemon_discards_in_flight_singulars_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/discards/failed-in-flight-singulars-when-daemon-disabled",
                   test_daemon_discards_failed_in_flight_singulars_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/discards/persistent-cache-when-daemon-disabled",
                   test_daemon_discards_persistent_cache_when_daemon_disabled);
  ADD_DAEMON_TEST ("/daemon/flushes-to-persistent-cache-on-finalize",
                   test_daemon_flushes_to_persistent_cache_on_finalize);
  ADD_DAEMON_TEST ("/daemon/limits-network-upload-size",
                   test_daemon_limits_network_upload_size);

#undef ADD_DAEMON_TEST

  /* These tests need slightly different set-up */
  g_test_add ("/daemon/reinitializes-cache-on-key-file-error",
              Fixture, NULL, setup_most,
              test_daemon_reinitializes_cache_on_key_file_error,
              teardown);
  g_test_add ("/daemon/crashes-on-non-key-file-error",
              Fixture, NULL, setup_most,
              test_daemon_crashes_on_non_key_file_error,
              teardown);
  g_test_add ("/daemon/submits-aggregates-from-tally/on-startup",
              Fixture, NULL, setup_most,
              test_daemon_submits_aggregates_from_tally_on_startup,
              teardown);

  return g_test_run ();
}
