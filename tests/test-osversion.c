/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <ctype.h>
#include <glib.h>

#include <eosmetrics/emtr-osversion-private.h>
#include "run-tests.h"

#define MOCK_FILE_ENVVAR "_MOCK_ENDLESSOS_VERSION_FILE"
#define MOCK_FILE_CONTENTS \
  "<endlessos-version>\n" \
  "  <platform>1</platform>\n" \
  "  <minor>2</minor>\n" \
  "  <micro>0</micro>\n" \
  "  <distributor>Endless Mobile</distributor>\n" \
  "  <date>2013-11-27</date>\n" \
  "</endlessos-version>"

static void
test_osversion_returns_version (void)
{
  /* Set up mock version file */
  const gchar *version_filename = g_getenv (MOCK_FILE_ENVVAR);

  g_assert (g_file_set_contents (version_filename, MOCK_FILE_CONTENTS, -1,
                                 NULL));

  gchar *version = emtr_get_os_version ();
  g_assert (version != NULL);
  g_assert_cmpstr(version, ==, "1.2.0");
  g_free (version);
}

static void
test_osversion_returns_null_on_error (void)
{
  const gchar *version_filename = g_getenv (MOCK_FILE_ENVVAR);
  g_assert (g_file_set_contents (version_filename, "", 0, NULL));
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
  if (g_getenv (MOCK_FILE_ENVVAR))
    {
      g_test_add_func ("/osversion/returns-version",
                       test_osversion_returns_version);
      g_test_add_func ("/osversion/returns-null-on-error",
                       test_osversion_returns_null_on_error);
      g_test_add_func ("/osversion/returns-null-on-error/subprocess",
                       test_osversion_returns_null_on_error_subprocess);
    }
}