/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-boot-id-provider.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <uuid/uuid.h>

#define TESTING_FILE_PATH "testing_boot_id_XXXXXX"
#define FIRST_TESTING_ID  "67ba25f5-b7af-48f9-a746-d1421a7e49de\n"
#define SECOND_TESTING_ID "1a4f1bfe-262f-4800-826d-d8e5b9d60081\n"


/*
 * The expected size in bytes of the file located at
 * /proc/sys/kernel/random/boot_id. The file should be 32 lower-case hexadecimal
 * characters interspersed with 4 hyphens and terminated with a newline
 * character.
 *
 * Exact format: "%08x-%04x-%04x-%04x-%012x\n"
 */
#define FILE_LENGTH 37

// Helper Functions

struct Fixture
{
  EmerBootIdProvider *id_provider;
  GFile *tmp_file;
};

static void
write_testing_boot_id (struct Fixture *fixture,
                     const gchar    *testing_id)
{
  g_assert (g_file_replace_contents (fixture->tmp_file, testing_id, FILE_LENGTH,
                                     NULL, FALSE,
                                     G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                     NULL, NULL));
}

static void
setup (struct Fixture *fixture,
       gconstpointer   unused)
{
  GFileIOStream *stream;
  fixture->tmp_file = g_file_new_tmp (TESTING_FILE_PATH, &stream, NULL);
  g_assert (fixture->tmp_file != NULL);
  write_testing_boot_id (fixture, FIRST_TESTING_ID);
  gchar *path = g_file_get_path (fixture->tmp_file);
  fixture->id_provider = emer_boot_id_provider_new_full (path);
  g_free (path);
}

static void
teardown (struct Fixture *fixture,
          gconstpointer   unused)
{
  g_object_unref (fixture->tmp_file);
  g_object_unref (fixture->id_provider);
}

// Testing Cases

static void
test_boot_id_provider_new_succeeds (struct Fixture *fixture,
                                    gconstpointer   unused)
{
  g_assert (fixture->id_provider != NULL);
}

static void
test_boot_id_provider_can_get_id (struct Fixture *fixture,
                                  gconstpointer   unused)
{
  uuid_t real_id;
  g_assert (emer_boot_id_provider_get_id (fixture->id_provider, real_id));

  gchar* testing_id_string = g_strdup (FIRST_TESTING_ID);
  g_strchomp (testing_id_string);
  uuid_t testing_id;
  uuid_parse (testing_id_string, testing_id);
  g_free (testing_id_string);

  g_assert (uuid_compare (testing_id, real_id) == 0);
}

static void
test_boot_id_provider_caches_id (struct Fixture *fixture,
                                 gconstpointer   unused)
{
  uuid_t real_id;
  g_assert (emer_boot_id_provider_get_id (fixture->id_provider, real_id));

  // If the boot id provider isn't caching its value, it will read this instead.
  write_testing_boot_id (fixture, SECOND_TESTING_ID);

  gchar* testing_id_string = g_strdup (SECOND_TESTING_ID);
  g_strchomp (testing_id_string);
  uuid_t testing_id;
  uuid_parse (testing_id_string, testing_id);
  g_free (testing_id_string);

  g_assert (uuid_compare (testing_id, real_id) != 0);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);
// gboolean is being used as a dummy fixture.
#define ADD_BOOT_TEST_FUNC(path, func) \
  g_test_add ((path), struct Fixture, NULL, setup, (func), teardown)

  ADD_BOOT_TEST_FUNC ("/boot-id-provider/new-succeeds",
                      test_boot_id_provider_new_succeeds);
  ADD_BOOT_TEST_FUNC ("/boot-id-provider/can-get-id",
                      test_boot_id_provider_can_get_id);
  ADD_BOOT_TEST_FUNC ("/boot-id-provider/caches-id",
                      test_boot_id_provider_caches_id);

#undef ADD_BOOT_TEST_FUNC

  return g_test_run ();
}
