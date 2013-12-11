/* Copyright 2013 Endless Mobile, Inc. */

#include <glib.h>
#include <eosmetrics/eosmetrics.h>

#include "run-tests.h"

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  add_osversion_tests ();
  add_uuid_tests ();
  add_web_tests ();
  add_connection_tests ();

  return g_test_run ();
}
