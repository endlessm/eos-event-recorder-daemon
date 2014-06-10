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

// Helper methods first:

static EmerDaemon*
make_daemon_for_testing (void)
{
  EmerMachineIdProvider *id_prov =
    emer_machine_id_provider_new (MACHINE_ID_PATH);
  return emer_daemon_new_full (42, // Version number
                               "test", // Environment
                               5,  // Network Send Interval
                               "http://localhost:8080", // uri, (port TBD) TODO
                               id_prov, // MachineIdProvider
                               20); // Buffer length
}

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

// Unit Tests second:

static void
test_daemon_new_succeeds (void)
{
  EmerDaemon *daemon = emer_daemon_new ();
  g_assert (daemon != NULL);
  g_object_unref (daemon);
}

static void
test_daemon_new_full_succeeds (void)
{
  EmerDaemon *daemon = make_daemon_for_testing ();
  g_assert (daemon != NULL);
  g_object_unref (daemon);
}

static void
test_daemon_can_record_singular_event (void)
{
  EmerDaemon *daemon = make_daemon_for_testing ();
  emer_daemon_record_singular_event (daemon,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     g_variant_new_string ("This must be ignored."));
  emer_daemon_record_singular_event (daemon,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     RELATIVE_TIMESTAMP,
                                     FALSE,
                                     g_variant_new_string ("This must be ignored."));
  emer_daemon_record_singular_event (daemon,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     RELATIVE_TIMESTAMP,
                                     TRUE,
                                     make_variant_payload ());
  g_object_unref (daemon);
}

static void
test_daemon_can_record_aggregate_events (void)
{
  EmerDaemon *daemon = make_daemon_for_testing ();
  emer_daemon_record_aggregate_event (daemon,
                                      USER_ID,
                                      make_event_id_gvariant (),
                                      101,
                                      RELATIVE_TIMESTAMP,
                                      FALSE,
                                      g_variant_new_string ("This must be ignored."));
  emer_daemon_record_aggregate_event (daemon,
                                      USER_ID,
                                      make_event_id_gvariant (),
                                      101,
                                      RELATIVE_TIMESTAMP,
                                      TRUE,
                                      make_variant_payload ());
  g_object_unref (daemon);
}

static void
test_daemon_can_record_event_sequence (void)
{
  EmerDaemon *daemon = make_daemon_for_testing ();
  emer_daemon_record_event_sequence (daemon,
                                     USER_ID,
                                     make_event_id_gvariant (),
                                     make_event_values_gvariant ());
  g_object_unref (daemon);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

  g_test_add_func ("/daemon/new-succeeds",
                   test_daemon_new_succeeds);
  g_test_add_func ("/daemon/new-full-succeeds",
                   test_daemon_new_full_succeeds);
  g_test_add_func ("/daemon/can-record-singular-event",
                   test_daemon_can_record_singular_event);
  g_test_add_func ("/daemon/can-record-aggregate-events",
                   test_daemon_can_record_aggregate_events);
  g_test_add_func ("/daemon/can-record-event-sequence",
                   test_daemon_can_record_event_sequence);

  return g_test_run ();
}