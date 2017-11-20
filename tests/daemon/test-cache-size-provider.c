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

// Helper Functions

typedef struct Fixture
{
  GFile *tmp_file;
  gchar *tmp_path;
} Fixture;

static void
write_cache_size_file (Fixture *fixture,
                       gchar   *key_file_data)
{
  gboolean ret;
  g_autoptr(GError) error = NULL;

  ret = g_file_set_contents (fixture->tmp_path, key_file_data,
                             -1, &error);
  g_assert_no_error (error);
  g_assert_true (ret);
}

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  g_autoptr(GFileIOStream) stream = NULL;

  fixture->tmp_file = g_file_new_tmp (CACHE_SIZE_FILE_PATH, &stream, NULL);
  fixture->tmp_path = g_file_get_path (fixture->tmp_file);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_clear_object (&fixture->tmp_file);
  g_unlink (fixture->tmp_path);
  g_free (fixture->tmp_path);
}

static void
test_cache_size_provider_can_get_max_cache_size (Fixture      *fixture,
                                                 gconstpointer unused)
{
  write_cache_size_file (fixture, FIRST_CACHE_SIZE_FILE_CONTENTS);

  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (fixture->tmp_path);
  g_assert_cmpint (max_cache_size, ==, FIRST_MAX_CACHE_SIZE);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);
#define ADD_CACHE_SIZE_TEST_FUNC(path, func) \
  g_test_add ((path), Fixture, NULL, setup, (func), teardown)

  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/can-get-max-cache-size",
                            test_cache_size_provider_can_get_max_cache_size);

#undef ADD_CACHE_SIZE_TEST_FUNC

  return g_test_run ();
}
