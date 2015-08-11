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

#include <uuid/uuid.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#define HYPHENS_IN_ID 4

#define TESTING_FILE_PATH "/tmp/testing-machine-id"
#define TESTING_ID        "04448f74fde24bd7a16f8da17869d5c3\n"
/*
 * The expected size in bytes of the file located at
 * #EmerMachineIdProvider:path.
 * According to http://www.freedesktop.org/software/systemd/man/machine-id.html
 * the file should be 32 lower-case hexadecimal characters followed by a
 * newline character.
 */
#define FILE_LENGTH 33


// Helper Functiobns

static gboolean
write_testing_machine_id ()
{
  GFile *file = g_file_new_for_path (TESTING_FILE_PATH);
  gboolean success = g_file_replace_contents (file,
                                              TESTING_ID,
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
setup (gboolean     *unused,
       gconstpointer dontuseme)
{
  g_unlink (TESTING_FILE_PATH);
  write_testing_machine_id ();
}

static void
teardown (gboolean     *unused,
          gconstpointer dontuseme)
{
  g_unlink (TESTING_FILE_PATH);
}

// Testing Cases

static void
test_machine_id_provider_new_succeeds (gboolean     *unused,
                                       gconstpointer dontuseme)
{
  EmerMachineIdProvider *id_provider =
    emer_machine_id_provider_new ();
  g_assert (id_provider != NULL);
  g_object_unref (id_provider);
}

static void
test_machine_id_provider_can_get_id (gboolean     *unused,
                                     gconstpointer dontuseme)
{
  EmerMachineIdProvider *id_provider =
    emer_machine_id_provider_new_full (TESTING_FILE_PATH);
  uuid_t id;
  g_assert (emer_machine_id_provider_get_id (id_provider, id));
  gchar unparsed_id[HYPHENS_IN_ID + FILE_LENGTH];
  uuid_unparse_lower (id, unparsed_id);
  gchar* unhypenated_id = unhyphenate_uuid (unparsed_id);
  g_assert_cmpstr (TESTING_ID, ==, unhypenated_id);
  g_free (unhypenated_id);
  g_object_unref (id_provider);
}

int
main (int                argc,
      const char * const argv[])
{
// We are using a gboolean as a fixture type, but it will go unused.
#define ADD_CACHE_TEST_FUNC(path, func) \
  g_test_add((path), gboolean, NULL, setup, (func), teardown)

  g_test_init (&argc, (char ***) &argv, NULL);

  ADD_CACHE_TEST_FUNC ("/machine-id-provider/new-succeeds",
                       test_machine_id_provider_new_succeeds);
  ADD_CACHE_TEST_FUNC ("/machine-id-provider/can-get-id",
                       test_machine_id_provider_can_get_id);

#undef ADD_CACHE_TEST_FUNC
  return g_test_run ();
}
