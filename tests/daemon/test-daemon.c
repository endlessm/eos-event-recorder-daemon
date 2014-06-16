/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-daemon.h"
#include "emer-machine-id-provider.h"

#include <uuid/uuid.h>
#include "shared/metrics-util.h"

#define MEANINGLESS_EVENT "350ac4ff-3026-4c25-9e7e-e8103b4fd5d8"
#define MEANINGLESS_EVENT_2 "d936cd5c-08de-4d4e-8a87-8df1f4a33cba"

#define MACHINE_ID_PATH "/tmp/testing-machine-id"
#define USER_ID 4200u
#define RELATIVE_TIMESTAMP G_GINT64_CONSTANT (123456789)

typedef struct
{
  EmerDaemon *test_object;
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

// Setup/Teardown functions next:

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  EmerMachineIdProvider *id_prov =
    emer_machine_id_provider_new (MACHINE_ID_PATH);
  fixture->test_object =
    emer_daemon_new_full (42, // Version number
                          "test", // Environment
                          5,  // Network Send Interval
                          "http://localhost:8080", // uri, (port TBD) TODO
                          id_prov, // MachineIdProvider
                          20); // Buffer length
  g_object_unref (id_prov);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_object_unref (fixture->test_object);
}

// Unit Tests next:

static void
test_daemon_new_succeeds (void)
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

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

  g_test_add_func ("/daemon/new-succeeds",
                   test_daemon_new_succeeds);

#define ADD_DAEMON_TEST(path, test_func) \
  g_test_add ((path), Fixture, NULL, setup, (test_func), teardown)

  ADD_DAEMON_TEST ("/daemon/new-full-succeeds",
                   test_daemon_new_full_succeeds);
  ADD_DAEMON_TEST ("/daemon/can-record-singular-event",
                   test_daemon_can_record_singular_event);
  ADD_DAEMON_TEST ("/daemon/can-record-aggregate-events",
                   test_daemon_can_record_aggregate_events);
  ADD_DAEMON_TEST ("/daemon/can-record-event-sequence",
                   test_daemon_can_record_event_sequence);

#undef ADD_DAEMON_TEST

  return g_test_run ();
}