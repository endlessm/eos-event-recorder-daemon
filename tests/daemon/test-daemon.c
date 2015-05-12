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

#include <glib.h>
#include <glib/gstdio.h>
#include <uuid/uuid.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#define MEANINGLESS_EVENT "350ac4ff-3026-4c25-9e7e-e8103b4fd5d8"
#define MEANINGLESS_EVENT_2 "d936cd5c-08de-4d4e-8a87-8df1f4a33cba"

#define MACHINE_ID_PATH "/tmp/testing-machine-id"
#define USER_ID 4200u
#define IO_OPERATION_TIMEOUT_MS 5000  /* 5 seconds */
#define RELATIVE_TIMESTAMP G_GINT64_CONSTANT (123456789)
#define EXPECTED_INHIBIT_SHUTDOWN_ARGS \
  "\"shutdown\" " \
  "\"EndlessOS Event Recorder Daemon\" " \
  "\"Flushing events to disk\" " \
  "\"delay\""

typedef struct
{
  EmerDaemon *test_object;
  EmerNetworkSendProvider *mock_network_send_prov;
  EmerPermissionsProvider *mock_permissions_prov;
  EmerPersistentCache *mock_persistent_cache;

  /* Mock logind service */
  GSubprocess *logind_mock;
  GString *logind_line;

  GMainLoop *main_loop;
  guint timeout_id;
} Fixture;

// Helper methods first:

static GVariant *
make_event_id_gvariant (void)
{
  uuid_t uuid;
  if (uuid_parse (MEANINGLESS_EVENT, uuid) != 0)
    g_error ("Failed to parse testing uuid.");
  GVariantBuilder event_id_builder;
  get_uuid_builder (uuid, &event_id_builder);
  return g_variant_builder_end (&event_id_builder);
}

static GVariant *
make_variant_payload (void)
{
  GVariant *sword_of_a_thousand = g_variant_new_boolean (TRUE);
  return g_variant_new_variant (sword_of_a_thousand);
}

static GVariant *
make_event_values_gvariant (void)
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
  return g_variant_new ("a(xbv)", &builder);
}

static void
start_mock_logind_service (Fixture *fixture)
{
  fixture->logind_mock = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL,
                                           "python3", "-m", "dbusmock",
                                           "--system", "--template", "logind",
                                           NULL);
  g_assert_nonnull (fixture->logind_mock);
}

static void
terminate_mock_logind_service_and_wait (Fixture *fixture)
{
  GError *error = NULL;

  g_subprocess_send_signal (fixture->logind_mock, SIGTERM);

  /*
   * Make sure it was the SIGTERM that finished the process, and not something
   * else.
   */
  g_assert_false (g_subprocess_wait_check (fixture->logind_mock, NULL, &error));
  g_assert_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "Child process killed by signal 15");

  g_object_unref (fixture->logind_mock);
}

static GPollableInputStream *
get_pollable_input_stream (GSubprocess *subprocess)
{
  GInputStream *input_stream = g_subprocess_get_stdout_pipe (subprocess);
  GPollableInputStream *pollable_input_stream =
    G_POLLABLE_INPUT_STREAM (input_stream);
  g_assert (g_pollable_input_stream_can_poll (pollable_input_stream));
  return pollable_input_stream;
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

/*
 * Append 1 byte from the given stream to the given byte array without blocking.
 * Returns TRUE if a character other than a newline was successfully appended.
 * Returns FALSE if a byte can't be obtained from the given stream without
 * blocking or a newline is read.
 */
static gboolean
append_byte (GPollableInputStream *pollable_input_stream,
             GString              *line)
{
  guint8 byte;
  GError *error = NULL;
  gssize num_bytes_read =
    g_pollable_input_stream_read_nonblocking (pollable_input_stream, &byte, 1,
                                              NULL /* GCancellable */, &error);
  switch (num_bytes_read)
    {
    case -1:
      g_assert (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK));
      g_error_free (error); // Fall through.
    case 0:
      return FALSE;
    case 1:
      g_assert (byte != '\0');
      g_string_append_c (line, byte);
      return byte != '\n';
    default:
      g_assert_not_reached ();
    }
}

/*
 * Append 1 line from the given stream to the given byte array without blocking.
 * Returns TRUE if a full line was successfully appended. Returns FALSE if less
 * than 1 line can be obtained from the given stream without blocking and
 * appends whatever data was available for immediate consumption.
 */
static gboolean
append_line (GPollableInputStream *pollable_input_stream,
             GString              *line)
{
  while (append_byte (pollable_input_stream, line)) {}
  return line->str[line->len - 1] == '\n';
}

static gboolean
on_output_received (GPollableInputStream *pollable_input_stream,
                    Fixture              *fixture)
{
  while (append_line (pollable_input_stream, fixture->logind_line))
    {
      gboolean shutdown_inhibited =
        contains_dbus_call (fixture->logind_line->str, "Inhibit",
                            EXPECTED_INHIBIT_SHUTDOWN_ARGS);
      if (shutdown_inhibited)
        {
          g_source_remove (fixture->timeout_id);
          g_main_loop_quit (fixture->main_loop);
          return G_SOURCE_REMOVE;
        }

      g_string_truncate (fixture->logind_line, 0);
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
timeout (gpointer unused)
{
  g_assert_not_reached ();
}

static void
await_shutdown_inhibit (Fixture *fixture)
{
  GPollableInputStream *logind_pollable_stream =
    get_pollable_input_stream (fixture->logind_mock);
  GSource *stdout_source =
    g_pollable_input_stream_create_source (logind_pollable_stream,
                                           NULL /* GCancellable */);

  fixture->main_loop = g_main_loop_new (NULL /* GMainContext */, FALSE);
  fixture->logind_line = g_string_new ("");
  g_source_set_callback (stdout_source, (GSourceFunc) on_output_received,
                         fixture, NULL /* GDestroyNotify */);
  g_source_attach (stdout_source, NULL /* GMainContext */);
  g_source_unref (stdout_source);

  fixture->timeout_id =
    g_timeout_add_seconds (5, timeout, NULL /* user data */);

  g_main_loop_run (fixture->main_loop);

  g_main_loop_unref (fixture->main_loop);
  g_string_free (fixture->logind_line, TRUE);
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
                                 IO_OPERATION_TIMEOUT_MS,
                                 NULL, NULL);
  g_assert_nonnull (response);
}

// Setup/Teardown functions next:

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  EmerMachineIdProvider *id_prov =
    emer_machine_id_provider_new_full (MACHINE_ID_PATH);
  fixture->mock_permissions_prov = emer_permissions_provider_new ();
  fixture->mock_persistent_cache = emer_persistent_cache_new (NULL, NULL);
  fixture->mock_network_send_prov = emer_network_send_provider_new ();
  fixture->test_object =
    emer_daemon_new_full (g_rand_new_with_seed (18),
                          5, // Network Send Interval
                          id_prov, // MachineIdProvider
                          fixture->mock_network_send_prov,
                          fixture->mock_permissions_prov,
                          fixture->mock_persistent_cache,
                          20); // Buffer length
  g_object_unref (id_prov);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_object_unref (fixture->test_object);
  g_object_unref (fixture->mock_network_send_prov);
  g_object_unref (fixture->mock_permissions_prov);
  g_object_unref (fixture->mock_persistent_cache);
  g_unlink (MACHINE_ID_PATH);
}

// Unit Tests next:

static void
test_daemon_new_succeeds (Fixture      *fixture,
                          gconstpointer unused)
{
  EmerDaemon *daemon = emer_daemon_new ();
  g_assert (daemon != NULL);
  g_object_unref (daemon);
}

static void
test_daemon_new_full_succeeds (Fixture      *fixture,
                               gconstpointer unused)
{
  g_assert (fixture->test_object != NULL);
}

static void
test_daemon_can_record_singular_event (Fixture      *fixture,
                                       gconstpointer unused)
{
  emer_daemon_record_singular_event (fixture->test_object,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     g_variant_new_string ("This must be ignored."));
  emer_daemon_record_singular_event (fixture->test_object,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     g_variant_new_string ("This must be ignored."));
  emer_daemon_record_singular_event (fixture->test_object,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     RELATIVE_TIMESTAMP,
                                     TRUE,
                                     make_variant_payload ());
}

static void
test_daemon_can_record_aggregate_events (Fixture      *fixture,
                                         gconstpointer unused)
{
  emer_daemon_record_aggregate_event (fixture->test_object,
                                      USER_ID,
                                      make_event_id_gvariant (),
                                      101,
                                      RELATIVE_TIMESTAMP,
                                      FALSE,
                                      g_variant_new_string ("This must be ignored."));
  emer_daemon_record_aggregate_event (fixture->test_object,
                                      USER_ID,
                                      make_event_id_gvariant (),
                                      101,
                                      RELATIVE_TIMESTAMP,
                                      TRUE,
                                      make_variant_payload ());
}

static void
test_daemon_can_record_event_sequence (Fixture      *fixture,
                                       gconstpointer unused)
{
  emer_daemon_record_event_sequence (fixture->test_object,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     make_event_values_gvariant ());
}

static void
test_daemon_does_not_record_singular_event_if_not_allowed (Fixture      *fixture,
                                                           gconstpointer unused)
{
  gint num_calls_before =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_prov, FALSE);
  test_daemon_can_record_singular_event (fixture, unused);

  gint num_calls_after =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  /*
   * FIXME: Nothing can currently be asserted about whether the EmerDaemon tries
   * to send its metrics, but at least we can confirm that it read the enabled
   * property.
   */
  g_assert_cmpint (num_calls_after, >=, num_calls_before + 1);
}

static void
test_daemon_does_not_record_aggregate_event_if_not_allowed (Fixture      *fixture,
                                                            gconstpointer unused)
{
  gint num_calls_before =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_prov, FALSE);
  test_daemon_can_record_aggregate_events (fixture, unused);

  gint num_calls_after =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  /* FIXME: See note above. */
  g_assert_cmpint (num_calls_after, >=, num_calls_before + 1);
}

static void
test_daemon_does_not_record_event_sequence_if_not_allowed (Fixture      *fixture,
                                                           gconstpointer unused)
{
  gint num_calls_before =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_prov, FALSE);
  test_daemon_can_record_event_sequence (fixture, unused);

  gint num_calls_after =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  /* FIXME: See note above. */
  g_assert_cmpint (num_calls_after, >=, num_calls_before + 1);
}

static void
test_daemon_inhibits_shutdown (Fixture      *fixture,
                               gconstpointer unused)
{
  start_mock_logind_service (fixture);
  await_shutdown_inhibit (fixture);
  terminate_mock_logind_service_and_wait (fixture);
}

static void
test_daemon_updates_timestamps_on_shutdown (Fixture      *fixture,
                                            gconstpointer unused)
{
  start_mock_logind_service (fixture);

  gint num_timestamp_updates_before =
    mock_persistent_cache_get_num_timestamp_updates (fixture->mock_persistent_cache);

  await_shutdown_inhibit (fixture);
  emit_shutdown_signal (TRUE);

  // Wait for EmerDaemon to handle the signal.
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, TRUE);

  gint num_timestamp_updates_after =
    mock_persistent_cache_get_num_timestamp_updates (fixture->mock_persistent_cache);
  g_assert_cmpint (num_timestamp_updates_after, ==, num_timestamp_updates_before + 1);

  terminate_mock_logind_service_and_wait (fixture);
}

static void
test_daemon_flushes_to_persistent_cache_on_shutdown (Fixture      *fixture,
                                                     gconstpointer unused)
{
  start_mock_logind_service (fixture);

  gint num_calls_before =
    mock_persistent_cache_get_store_metrics_called (fixture->mock_persistent_cache);

  await_shutdown_inhibit (fixture);
  emit_shutdown_signal (TRUE);

  // Wait for EmerDaemon to handle the signal.
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, TRUE);

  gint num_calls_after =
    mock_persistent_cache_get_store_metrics_called (fixture->mock_persistent_cache);
  g_assert_cmpint (num_calls_after, ==, num_calls_before + 1);

  terminate_mock_logind_service_and_wait (fixture);
}

static void
test_daemon_reinhibits_shutdown_on_shutdown_cancel (Fixture      *fixture,
                                                    gconstpointer unused)
{
  start_mock_logind_service (fixture);

  await_shutdown_inhibit (fixture);
  emit_shutdown_signal (TRUE);

  // Wait for EmerDaemon to handle the signal.
  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, TRUE);

  emit_shutdown_signal (FALSE);
  await_shutdown_inhibit (fixture);

  terminate_mock_logind_service_and_wait (fixture);
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
  ADD_DAEMON_TEST ("/daemon/can-record-singular-event",
                   test_daemon_can_record_singular_event);
  ADD_DAEMON_TEST ("/daemon/can-record-aggregate-events",
                   test_daemon_can_record_aggregate_events);
  ADD_DAEMON_TEST ("/daemon/can-record-event-sequence",
                   test_daemon_can_record_event_sequence);
  ADD_DAEMON_TEST ("/daemon/does-not-record-singular-event-if-not-allowed",
                   test_daemon_does_not_record_singular_event_if_not_allowed);
  ADD_DAEMON_TEST ("/daemon/does-not-record-aggregate-event-if-not-allowed",
                   test_daemon_does_not_record_aggregate_event_if_not_allowed);
  ADD_DAEMON_TEST ("/daemon/does-not-record-event-sequence-if-not-allowed",
                   test_daemon_does_not_record_event_sequence_if_not_allowed);
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
