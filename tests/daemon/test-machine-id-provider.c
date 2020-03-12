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

#include "emer-machine-id-provider.h"

#include <string.h>

#include <uuid/uuid.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#define HYPHENS_IN_ID 4

#define TESTING_BASE_TEMPLATE "emer-machine-id-provider-tmp-XXXXXX"
#define TESTING_TRACKING_ID        "d17b0fd3b28e4302bcd81ab471e06de9\n"
#define TESTING_MALFORMED_TRACKING_ID        "absoluterubbish\n"

/* The expected size in characters of the hexadecimal representation of a
 * metrics ID, without any trailing newline.
 */
#define TRACKING_ID_LENGTH 32

/*
 * The expected size in bytes of the file located at
 * #EmerMachineIdProvider:path.
 * According to http://www.freedesktop.org/software/systemd/man/machine-id.html
 * the file should be 32 lower-case hexadecimal characters followed by a
 * newline character.
 */
#define FILE_LENGTH 33


typedef struct {
  gchar *test_temp_path;
  gchar *tracking_id_file_path;
} MachineIdTestFixture;

// Helper Functions

static gboolean
write_testing_machine_id (const gchar *path, const gchar *id)
{
  GFile *file = g_file_new_for_path (path);
  gboolean success = g_file_replace_contents (file,
                                              id,
                                              FILE_LENGTH,
                                              NULL,
                                              FALSE,
                                              G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE,
                                              NULL,
                                              NULL,
                                              NULL);
  if (!success)
    g_critical ("Testing code failed to write testing machine id.");
  g_object_unref (file);
  return success;
}

static gchar *
unhyphenate_uuid (gchar *uuid_with_hyphens)
{
  return g_strdup_printf ("%.8s%.4s%.4s%.4s%.12s\n", uuid_with_hyphens,
                          uuid_with_hyphens + 9, uuid_with_hyphens + 14,
                          uuid_with_hyphens + 19, uuid_with_hyphens + 24);
}

static void
setup (MachineIdTestFixture *fixture,
       gconstpointer         tdata)
{
  g_autoptr(GError) error = NULL;
  gint write_tracking_id_file_explicitly = GPOINTER_TO_INT (tdata);

  fixture->test_temp_path = g_dir_make_tmp (TESTING_BASE_TEMPLATE, &error);
  g_assert_no_error (error);

  fixture->tracking_id_file_path = g_build_filename (fixture->test_temp_path,
                                                     "tracking-id",
                                                     NULL);
  if (write_tracking_id_file_explicitly)
    write_testing_machine_id (fixture->tracking_id_file_path, TESTING_TRACKING_ID);
}

static void
teardown (MachineIdTestFixture *fixture,
          gconstpointer         tdata)
{
  g_unlink (fixture->tracking_id_file_path);
  g_rmdir (fixture->test_temp_path);

  g_free (fixture->tracking_id_file_path);
  g_free (fixture->test_temp_path);
}

// Testing Cases

static void
test_machine_id_provider_create_tracking_id_if_unavailable (MachineIdTestFixture *fixture,
                                                            gconstpointer         tdata)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *contents = NULL;

  g_assert_false (g_file_test (fixture->tracking_id_file_path, G_FILE_TEST_EXISTS));

  g_autoptr(EmerMachineIdProvider) id_provider =
    emer_machine_id_provider_new_full (fixture->tracking_id_file_path);

  uuid_t id;
  // id_provider_get_id will write a new tracking ID, if no ID is found.
  g_assert (emer_machine_id_provider_get_id (id_provider, NULL, id));

  g_assert (g_file_test (fixture->tracking_id_file_path, G_FILE_TEST_EXISTS));

  /* Read the tracking_id_file_path using g_file_get_contents
   * and check that its size matches what we would normally write to the
   * file */
  g_file_get_contents (fixture->tracking_id_file_path, &contents, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpint (strlen (contents), ==, strlen (TESTING_TRACKING_ID));
}

static void
test_machine_id_provider_new_succeeds (MachineIdTestFixture *fixture,
                                       gconstpointer         tdata)
{
  EmerMachineIdProvider *id_provider =
    emer_machine_id_provider_new ();
  g_assert (id_provider != NULL);
  g_object_unref (id_provider);
}

static void
test_machine_id_provider_can_get_tracking_id (MachineIdTestFixture *fixture,
                                              gconstpointer         tdata)
{
  g_autoptr(EmerMachineIdProvider) id_provider =
    emer_machine_id_provider_new_full (fixture->tracking_id_file_path);
  uuid_t id;

  // simultaneous check if we can get a unparsed_id directly to show in UI
  g_autofree gchar *unparsed_id_direct = NULL;

  g_assert (emer_machine_id_provider_get_id (id_provider, &unparsed_id_direct, id));
  gchar unparsed_id[HYPHENS_IN_ID + FILE_LENGTH];
  uuid_unparse_lower (id, unparsed_id);
  g_autofree gchar* unhypenated_id = unhyphenate_uuid (unparsed_id);
  g_assert_cmpstr (TESTING_TRACKING_ID, ==, unhypenated_id);

  g_assert_cmpuint (strlen (unparsed_id_direct), ==, TRACKING_ID_LENGTH);
  g_assert_cmpmem (TESTING_TRACKING_ID, TRACKING_ID_LENGTH, unparsed_id_direct, TRACKING_ID_LENGTH);
}

static void
test_machine_id_provider_writes_correctly_formed_tracking_id (MachineIdTestFixture *fixture,
                                                              gconstpointer         tdata)
{
  g_autoptr(EmerMachineIdProvider) id_provider =
    emer_machine_id_provider_new_full (fixture->tracking_id_file_path);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *contents = NULL;

  emer_machine_id_provider_reset_tracking_id (id_provider, &error);
  g_assert_no_error (error);

  /* Read the tracking_id_file_path using g_file_get_contents
   * and check that its size matches what we would normally write to the
   * file */
  g_file_get_contents (fixture->tracking_id_file_path, &contents, NULL, &error);
  g_assert_no_error (error);

  g_assert_cmpint (strlen (contents), ==, strlen (TESTING_TRACKING_ID));

  /* Double check that ID is retriveable with provider_get_id ()
   * and is different from TESTING_TRACKING_ID */
  uuid_t id;
  g_assert (emer_machine_id_provider_get_id (id_provider, NULL, id));
  gchar unparsed_id[HYPHENS_IN_ID + FILE_LENGTH];
  uuid_unparse_lower (id, unparsed_id);
  g_autofree gchar* unhypenated_id = unhyphenate_uuid (unparsed_id);
  g_assert_cmpstr (TESTING_TRACKING_ID, !=, unhypenated_id);
}

static void
test_machine_id_provider_read_malformed_tracking_id (MachineIdTestFixture *fixture,
                                                     gconstpointer         tdata)
{
  g_autoptr(EmerMachineIdProvider) id_provider =
    emer_machine_id_provider_new_full (fixture->tracking_id_file_path);
  uuid_t id;
  write_testing_machine_id (fixture->tracking_id_file_path,
                            TESTING_MALFORMED_TRACKING_ID);
  // Fails to read because tracking ID is malformed
  g_assert_false (emer_machine_id_provider_get_id (id_provider, NULL, id));
}

gint
main (gint                argc,
      const gchar * const argv[])
{
// We are using a gboolean as a fixture type, but it will go unused.
#define ADD_CACHE_TEST_FUNC(path, func, tdata) \
  g_test_add((path), MachineIdTestFixture, tdata, setup, (func), teardown)

#define WRITE_TRACKING_ID_FILE        GINT_TO_POINTER(TRUE)
#define DONT_WRITE_TRACKING_ID_FILE   GINT_TO_POINTER(FALSE)

  g_test_init (&argc, (gchar ***) &argv, NULL);

  ADD_CACHE_TEST_FUNC ("/machine-id-provider/new-succeeds",
                       test_machine_id_provider_new_succeeds,
		       WRITE_TRACKING_ID_FILE);
  ADD_CACHE_TEST_FUNC ("/machine-id-provider/can-get-tracking-id",
                       test_machine_id_provider_can_get_tracking_id,
		       WRITE_TRACKING_ID_FILE);
  ADD_CACHE_TEST_FUNC ("/machine-id-provider/can-write-correctly-formed-tracking-id",
                       test_machine_id_provider_writes_correctly_formed_tracking_id,
                       WRITE_TRACKING_ID_FILE);
  ADD_CACHE_TEST_FUNC ("/machine-id-provider/read-malformed-tracking-id",
                       test_machine_id_provider_read_malformed_tracking_id,
                       WRITE_TRACKING_ID_FILE);
  ADD_CACHE_TEST_FUNC ("/machine-id-provider/create-tracking-id-if-unavailable",
                       test_machine_id_provider_create_tracking_id_if_unavailable,
                       DONT_WRITE_TRACKING_ID_FILE);
#undef ADD_CACHE_TEST_FUNC
  return g_test_run ();
}
