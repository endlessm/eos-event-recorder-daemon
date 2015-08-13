/* Copyright 2015 Endless Mobile, Inc. */

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

#include "emer-cache-size-provider.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#define CACHE_SIZE_FILE_PATH "cache_size_file_XXXXXX"

#define FIRST_MAX_CACHE_SIZE 40

#define FIRST_CACHE_SIZE_FILE_CONTENTS \
 "[persistent_cache_size]\n" \
 "maximum=40\n"

#define SECOND_CACHE_SIZE_FILE_CONTENTS \
 "[persistent_cache_size]\n" \
 "maximum=3\n"

// Helper Functions

typedef struct Fixture
{
  EmerCacheSizeProvider *cache_size_provider;
  GFile *tmp_file;
  gchar *tmp_path;
  GKeyFile *key_file;
} Fixture;

static void
write_cache_size_file (Fixture *fixture,
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
  fixture->tmp_file = g_file_new_tmp (CACHE_SIZE_FILE_PATH, &stream, NULL);
  g_object_unref (stream);
  fixture->tmp_path = g_file_get_path (fixture->tmp_file);

  fixture->key_file = g_key_file_new ();
  write_cache_size_file (fixture, FIRST_CACHE_SIZE_FILE_CONTENTS);

  fixture->cache_size_provider =
    emer_cache_size_provider_new_full (fixture->tmp_path);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_key_file_unref (fixture->key_file);
  g_object_unref (fixture->tmp_file);
  g_unlink (fixture->tmp_path);
  g_free (fixture->tmp_path);
  g_object_unref (fixture->cache_size_provider);
}

static void
test_cache_size_provider_new_succeeds (Fixture      *fixture,
                                       gconstpointer unused)
{
  g_assert_nonnull (fixture->cache_size_provider);
}

static void
test_cache_size_provider_can_get_max_cache_size (Fixture      *fixture,
                                                 gconstpointer unused)
{
  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (fixture->cache_size_provider);
  g_assert_cmpint (max_cache_size, ==, FIRST_MAX_CACHE_SIZE);
}

static void
test_cache_size_provider_caches_max_cache_size (Fixture      *fixture,
                                                gconstpointer unused)
{
  // The first read should cache the max cache size.
  guint64 first_max_cache_size =
    emer_cache_size_provider_get_max_cache_size (fixture->cache_size_provider);
  g_assert_cmpint (first_max_cache_size, ==, FIRST_MAX_CACHE_SIZE);

  // This file should be ignored by the cache size provider.
  write_cache_size_file (fixture, SECOND_CACHE_SIZE_FILE_CONTENTS);

  // The second read should read from memory, not disk.
  guint64 second_max_cache_size =
    emer_cache_size_provider_get_max_cache_size (fixture->cache_size_provider);
  g_assert_cmpint (second_max_cache_size, ==, FIRST_MAX_CACHE_SIZE);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);
#define ADD_CACHE_SIZE_TEST_FUNC(path, func) \
  g_test_add ((path), Fixture, NULL, setup, (func), teardown)

  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/new-succeeds",
                            test_cache_size_provider_new_succeeds);
  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/can-get-max-cache-size",
                            test_cache_size_provider_can_get_max_cache_size);
  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/caches-max-cache-size",
                            test_cache_size_provider_caches_max_cache_size);

#undef ADD_CACHE_SIZE_TEST_FUNC

  return g_test_run ();
}
