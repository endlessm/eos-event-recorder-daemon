/* Copyright 2014, 2015 Endless Mobile, Inc. */

/* This file is part of eos-event-recorder-daemon.
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

#include "emer-cache-version-provider.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#define TESTING_FILE_PATH "testing_cache_version_XXXXXX"

#define STARTING_VERSION 40
#define STARTING_KEY_FILE \
 "[cache_version_info]\n" \
 "version=40\n"

#define SECOND_VERSION 42
#define SECOND_KEY_FILE \
 "[cache_version_info]\n" \
 "version=42\n"

// Helper Functions

typedef struct Fixture
{
  EmerCacheVersionProvider *version_provider;
  GFile *tmp_file;
  gchar *tmp_path;
  GKeyFile *key_file;
} Fixture;

static void
write_testing_cache_keyfile (Fixture *fixture,
                             gchar   *key_file_data)
{
  GError *error = NULL;
  g_key_file_load_from_data (fixture->key_file, key_file_data, -1,
                             G_KEY_FILE_NONE, &error);
  g_assert_no_error (error);

  g_key_file_save_to_file (fixture->key_file, fixture->tmp_path, &error);
  g_assert_no_error (error);
}

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  GFileIOStream *stream;
  fixture->tmp_file = g_file_new_tmp (TESTING_FILE_PATH, &stream, NULL);
  fixture->tmp_path = g_file_get_path (fixture->tmp_file);

  fixture->key_file = g_key_file_new ();
  write_testing_cache_keyfile (fixture, STARTING_KEY_FILE);

  fixture->version_provider =
    emer_cache_version_provider_new_full (fixture->tmp_path);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_key_file_unref (fixture->key_file);
  g_object_unref (fixture->tmp_file);
  g_unlink (fixture->tmp_path);
  g_free (fixture->tmp_path);
  g_object_unref (fixture->version_provider);
}

static void
test_cache_version_provider_new_succeeds (Fixture      *fixture,
                                          gconstpointer unused)
{
  g_assert (fixture->version_provider != NULL);
}

static void
test_cache_version_provider_can_get_version (Fixture      *fixture,
                                             gconstpointer unused)
{
  gint version;
  g_assert (emer_cache_version_provider_get_version (fixture->version_provider,
                                                     &version));
  g_assert_cmpint (version, ==, STARTING_VERSION);
}

static void
test_cache_version_provider_caches_version (Fixture      *fixture,
                                            gconstpointer unused)
{
  // First read should cache value.
  gint first_version;
  g_assert (emer_cache_version_provider_get_version (fixture->version_provider,
                                                     &first_version));
  g_assert_cmpint (first_version, ==, STARTING_VERSION);

  // This key_file should now be ignored by the version provider.
  write_testing_cache_keyfile (fixture, SECOND_KEY_FILE);

  // Second read should not read from disk.
  gint second_version;
  g_assert (emer_cache_version_provider_get_version (fixture->version_provider,
                                                     &second_version));

  // Should not have changed.
  g_assert_cmpint (second_version, ==, STARTING_VERSION);
}

static void
test_cache_version_provider_can_set_version (Fixture      *fixture,
                                             gconstpointer unused)
{
  // First read should cache value.
  gint first_version;
  g_assert (emer_cache_version_provider_get_version (fixture->version_provider,
                                                     &first_version));
  g_assert_cmpint (first_version, ==, STARTING_VERSION);

  // Cached value should be overwritten.
  gint write_version = first_version + 1;
  GError *error = NULL;
  g_assert (emer_cache_version_provider_set_version (fixture->version_provider,
                                                     write_version, &error));
  g_assert_no_error (error);

  gint second_version;
  g_assert (emer_cache_version_provider_get_version (fixture->version_provider,
                                                     &second_version));
  g_assert_cmpint (second_version, ==, write_version);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);
#define ADD_CACHE_VERSION_TEST_FUNC(path, func) \
  g_test_add ((path), Fixture, NULL, setup, (func), teardown)

  ADD_CACHE_VERSION_TEST_FUNC ("/cache-version-provider/new-succeeds",
                               test_cache_version_provider_new_succeeds);
  ADD_CACHE_VERSION_TEST_FUNC ("/cache-version-provider/can-get-version",
                               test_cache_version_provider_can_get_version);
  ADD_CACHE_VERSION_TEST_FUNC ("/cache-version-provider/caches-version",
                               test_cache_version_provider_caches_version);
  ADD_CACHE_VERSION_TEST_FUNC ("/cache-version-provider/can-set-version",
                               test_cache_version_provider_can_set_version);

#undef ADD_CACHE_VERSION_TEST_FUNC

  return g_test_run ();
}
