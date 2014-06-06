/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-daemon.h"
#include "emer-machine-id-provider.h"

#define TESTING_MACHINE_ID_PATH "/tmp/testing-machine-id"

// Helper methods first:

static EmerDaemon*
make_daemon_for_testing (void)
{
  EmerMachineIdProvider *id_prov =
    emer_machine_id_provider_new (TESTING_MACHINE_ID_PATH);
  return emer_daemon_new_full (42, // Version number
                               "test", // Environment
                               5,  // Network Send Interval
                               "http://localhost", // uri
                               id_prov, // MachineIdProvider
                               20); // Buffer length
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

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

  g_test_add_func ("/daemon/new-succeeds",
                   test_daemon_new_succeeds);
  g_test_add_func ("/daemon/new-full-succeeds",
                   test_daemon_new_full_succeeds);

  return g_test_run ();
}