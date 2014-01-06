/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <ctype.h>
#include <glib.h>

#include <eosmetrics/emtr-osversion-private.h>
#include "run-tests.h"

static void
test_osversion_returns_version (void)
{
  set_up_mock_version_file (MOCK_VERSION_FILE_CONTENTS);

  gchar *version = emtr_get_os_version ();
  g_assert (version != NULL);
  g_assert_cmpstr(version, ==, "1.2.0");
  g_free (version);
}

static void
test_osversion_returns_null_on_error (void)
{
  set_up_mock_version_file ("");
  /* Version file is empty here, so the call should return NULL to indicate an
  error, and print a critical message */
  g_test_trap_subprocess ("/osversion/returns-null-on-error/subprocess", 0, 0);
  g_test_trap_assert_stderr ("*CRITICAL*");
}

static void
test_osversion_returns_null_on_error_subprocess (void)
{
  gchar *version = emtr_get_os_version ();
  g_assert (version == NULL);
}

void
add_osversion_tests (void)
{
  /* Don't perform these tests on the real version file */
  if (g_getenv (MOCK_VERSION_FILE_ENVVAR))
    {
      g_test_add_func ("/osversion/returns-version",
                       test_osversion_returns_version);
      g_test_add_func ("/osversion/returns-null-on-error",
                       test_osversion_returns_null_on_error);
      g_test_add_func ("/osversion/returns-null-on-error/subprocess",
                       test_osversion_returns_null_on_error_subprocess);
    }
}