/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-machine-id-provider.h"

#include <glib.h>
#include <gio/gio.h>
#include <uuid/uuid.h>

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

// Testing Cases

static void
test_machine_id_provider_new_succeeds (void)
{
  write_testing_machine_id ();
  EmerMachineIdProvider *id_provider =
    emer_machine_id_provider_new ();
  g_assert (id_provider != NULL);
  g_object_unref (id_provider);
}

static void
test_machine_id_provider_can_get_id (void)
{
  write_testing_machine_id ();
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
  g_test_init (&argc, (char ***) &argv, NULL);

  g_test_add_func ("/machine-id-provider/new-succeeds",
                   test_machine_id_provider_new_succeeds);
  g_test_add_func ("/machine-id-provider/can-get-id",
                   test_machine_id_provider_can_get_id);

  return g_test_run ();
}
