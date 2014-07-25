/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-daemon.h"
#include "emer-boot-id-provider.h"
#include "emer-machine-id-provider.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"
#include "mock-permissions-provider.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <uuid/uuid.h>
#include <signal.h>
#include "shared/metrics-util.h"

#define MEANINGLESS_EVENT "350ac4ff-3026-4c25-9e7e-e8103b4fd5d8"
#define MEANINGLESS_EVENT_2 "d936cd5c-08de-4d4e-8a87-8df1f4a33cba"

#define MACHINE_ID_PATH "/tmp/testing-machine-id"
#define USER_ID 4200u
#define RELATIVE_TIMESTAMP G_GINT64_CONSTANT (123456789)

typedef struct
{
  EmerDaemon *test_object;
  EmerPermissionsProvider *mock_permissions_prov;
  EmerPersistentCache *mock_persistent_cache;

  /* Mock logind service */
  GSubprocess *logind_mock;

  /* Only used during setup() */
  guint watcher_id;
  guint watcher_timeout_id;
  GMainLoop *watcher_loop;
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
on_logind_name_appeared (GDBusConnection *connection,
                         const gchar     *name,
                         const gchar     *owner,
                         Fixture         *fixture)
{
  g_source_remove (fixture->watcher_timeout_id);
  g_bus_unwatch_name (fixture->watcher_id);
  g_main_loop_quit (fixture->watcher_loop);
}

static gboolean
on_logind_name_timeout (Fixture *fixture)
{
  g_assert_not_reached ();
}

static void
start_mock_logind_service_and_wait (Fixture *fixture)
{
  fixture->logind_mock = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, NULL,
                                           "python", "-m", "dbusmock",
                                           "--system", "--template", "logind",
                                           NULL);
  g_assert_nonnull (fixture->logind_mock);

  fixture->watcher_id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                                          "org.freedesktop.login1",
                                          G_BUS_NAME_WATCHER_FLAGS_NONE,
                                          (GBusNameAppearedCallback) on_logind_name_appeared,
                                          NULL,
                                          fixture, NULL);
  fixture->watcher_timeout_id =
    g_timeout_add_seconds (5, (GSourceFunc) on_logind_name_timeout, NULL);
  fixture->watcher_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (fixture->watcher_loop);

  g_main_loop_unref (fixture->watcher_loop);
}

static void
terminate_mock_logind_service_and_wait (Fixture *fixture)
{
  GError *error = NULL;

  g_subprocess_send_signal (fixture->logind_mock, SIGTERM);

  /* Make sure it was the SIGTERM that finished the process, and not something
  else. */
  g_assert_false (g_subprocess_wait_check (fixture->logind_mock, NULL, &error));
  g_assert_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==, "Child process killed by signal 15");
}

// Setup/Teardown functions next:

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  start_mock_logind_service_and_wait (fixture);

  EmerMachineIdProvider *id_prov =
    emer_machine_id_provider_new_full (MACHINE_ID_PATH);
  fixture->mock_permissions_prov = emer_permissions_provider_new ();
  fixture->mock_persistent_cache = emer_persistent_cache_new (NULL, NULL);
  fixture->test_object =
    emer_daemon_new_full (g_rand_new_with_seed (18),
                          5,  // Network Send Interval
                          "http://localhost/", // uri,
                          id_prov, // MachineIdProvider
                          fixture->mock_permissions_prov, // PermissionsProvider
                          fixture->mock_persistent_cache, // PersistentCache
                          20); // Buffer length
  g_object_unref (id_prov);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_object_unref (fixture->test_object);
  g_object_unref (fixture->mock_permissions_prov);
  g_object_unref (fixture->mock_persistent_cache);
  g_unlink (MACHINE_ID_PATH);

  terminate_mock_logind_service_and_wait (fixture);
  g_object_unref (fixture->logind_mock);
}

// Unit Tests next:

static void
test_daemon_new_succeeds (Fixture      *fixture,
                          gconstpointer unused)
{
  EmerDaemon *daemon = emer_daemon_new ("test");
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
  guint num_calls =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_prov, FALSE);
  test_daemon_can_record_singular_event (fixture, unused);

  /* FIXME: nothing can currently be asserted about whether the EmerDaemon tries
  to send its metrics, but at least we can confirm that it read the enabled
  property: */
  g_assert_cmpuint (mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov),
                    >=, num_calls + 1);
}

static void
test_daemon_does_not_record_aggregate_event_if_not_allowed (Fixture      *fixture,
                                                            gconstpointer unused)
{
  guint num_calls =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_prov, FALSE);
  test_daemon_can_record_aggregate_events (fixture, unused);

  /* FIXME: See note above. */
  g_assert_cmpuint (mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov),
                    >=, num_calls + 1);
}

static void
test_daemon_does_not_record_event_sequence_if_not_allowed (Fixture      *fixture,
                                                           gconstpointer unused)
{
  guint num_calls =
    mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov);

  emer_permissions_provider_set_daemon_enabled (fixture->mock_permissions_prov, FALSE);
  test_daemon_can_record_event_sequence (fixture, unused);

  /* FIXME: See note above. */
  g_assert_cmpuint (mock_permissions_provider_get_daemon_enabled_called (fixture->mock_permissions_prov),
                    >=, num_calls + 1);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

#define ADD_DAEMON_TEST(path, test_func) \
  g_test_add ((path), Fixture, NULL, setup, (test_func), teardown)

  ADD_DAEMON_TEST ("/daemon/new-succeeds", test_daemon_new_succeeds);
  ADD_DAEMON_TEST ("/daemon/new-full-succeeds",
                   test_daemon_new_full_succeeds);
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

#undef ADD_DAEMON_TEST

  return g_test_run ();
}
