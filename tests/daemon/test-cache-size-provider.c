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

#include "config.h"
#include "emer-cache-size-provider.h"

#include <string.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#define CACHE_SIZE_FILE_PATH "cache_size_file_XXXXXX"

#define FIRST_MAX_CACHE_SIZE 40
#define DEFAULT_MAX_CACHE_SIZE 10000000

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
write_cache_size_file (Fixture     *fixture,
                       const gchar *key_file_data,
                       gssize       key_file_size)
{
  gboolean ret;
  g_autoptr(GError) error = NULL;

  ret = g_file_set_contents (fixture->tmp_path, key_file_data, key_file_size,
                             &error);
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
  write_cache_size_file (fixture, FIRST_CACHE_SIZE_FILE_CONTENTS, -1);

  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (fixture->tmp_path);
  g_assert_cmpint (max_cache_size, ==, FIRST_MAX_CACHE_SIZE);
}

static void
assert_gets_default_max_cache_size (Fixture *fixture)
{
  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (fixture->tmp_path);
  g_assert_cmpint (max_cache_size, ==, DEFAULT_MAX_CACHE_SIZE);
}

static void
test_cache_size_provider_writes_file_if_missing (Fixture      *fixture,
                                                 gconstpointer unused)
{
  gboolean ret;
  g_autoptr(GError) error = NULL;

  ret = g_file_delete (fixture->tmp_file, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  assert_gets_default_max_cache_size (fixture);
}

static void
test_cache_size_provider_recovers_if_corrupt_empty (Fixture      *fixture,
                                                    gconstpointer unused)
{
  write_cache_size_file (fixture, "", 0);
  assert_gets_default_max_cache_size (fixture);
}

static void
test_cache_size_provider_recovers_if_corrupt_nul (Fixture      *fixture,
                                                  gconstpointer unused)
{
  /* If the target file does not already exist, then g_file_set_contents(), as
   * used by g_key_file_save_to_file(), followed by a poorly-timed system crash
   * can leave the target file at the corrent size but full of NUL bytes.
   *
   * https://bugzilla.gnome.org/show_bug.cgi?id=790638
   *
   * The key file appears to be empty when one tries to read it (since GKeyFile
   * reads the contents as a NUL-terminated string) so we should recover by
   * returning the default value.
   */
  g_test_bug ("T19953");

  gssize size = 41;
  gchar *contents = g_malloc0 (41);

  write_cache_size_file (fixture, contents, size);
  assert_gets_default_max_cache_size (fixture);
}

static void
test_cache_size_provider_recovers_if_corrupt_missing_key (Fixture      *fixture,
                                                          gconstpointer unused)
{
  /* If the file is not logically empty, but does not contain the expected
   * max_cache_size key, we still want to return the default value.
   * (Perhaps the file is destined
   * for a future version of the daemon which accepts some new field.)
   */
  const gchar *contents = "[persistent_cache_size]\nunrelated_key=1";

  write_cache_size_file (fixture, contents, -1);

  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (fixture->tmp_path);
  g_assert_cmpint (max_cache_size, ==, DEFAULT_MAX_CACHE_SIZE);
}

static void
test_cache_size_provider_recovers_if_corrupt_garbage (Fixture      *fixture,
                                                      gconstpointer unused)
{
  /* If the file exists but is malformed, we should log a warning */
  const gchar *contents = "i think i'm paranoid";

  write_cache_size_file (fixture, contents, -1);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*Key file*i think i'm paranoid*");
  assert_gets_default_max_cache_size (fixture);
  g_test_assert_expected_messages ();
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);
  g_test_bug_base ("https://phabricator.endlessm.com/");

#define ADD_CACHE_SIZE_TEST_FUNC(path, func) \
  g_test_add ((path), Fixture, NULL, setup, (func), teardown)

  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/can-get-max-cache-size",
                            test_cache_size_provider_can_get_max_cache_size);
  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/writes-file-if-missing",
                            test_cache_size_provider_writes_file_if_missing);
  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/recovers-if-corrupt/empty",
                            test_cache_size_provider_recovers_if_corrupt_empty);
  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/recovers-if-corrupt/nul",
                            test_cache_size_provider_recovers_if_corrupt_nul);
  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/recovers-if-corrupt/missing-key",
                            test_cache_size_provider_recovers_if_corrupt_missing_key);
  ADD_CACHE_SIZE_TEST_FUNC ("/cache-size-provider/recovers-if-corrupt/garbage",
                            test_cache_size_provider_recovers_if_corrupt_garbage);

#undef ADD_CACHE_SIZE_TEST_FUNC

  return g_test_run ();
}
