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

#include "emer-circular-file.h"

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

typedef struct _Fixture
{
  gchar *data_file_path;
} Fixture;

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  GFileIOStream *stream;
  GFile *data_file =
    g_file_new_tmp ("circular-file-XXXXXX.dat", &stream, NULL /* GError */);
  g_assert_nonnull (data_file);
  g_object_unref (stream);
  fixture->data_file_path = g_file_get_path (data_file);
  g_object_unref (data_file);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_unlink (fixture->data_file_path);
  gchar *metadata_file_path =
    g_strconcat (fixture->data_file_path, METADATA_EXTENSION, NULL);
  g_free (fixture->data_file_path);

  g_unlink (metadata_file_path);
  g_free (metadata_file_path);
}

static gsize
get_elem_size (const gchar *string)
{
  return strlen (string) + 1 /* terminal null byte */;
}

static gsize
get_total_elem_size (const gchar * const *strings,
                     gsize                num_strings)
{
  gsize total_elem_size = 0;
  for (gsize i = 0; i < num_strings; i++)
    total_elem_size += get_elem_size (strings[i]);

  return total_elem_size;
}

/* Returns the number of bytes the given string will consume when saved in a
 * circular file. See emer_circular_file_append for more details.
 */
static guint64
get_disk_size (const gchar *string)
{
  gsize elem_size = get_elem_size (string);
  return sizeof (guint64) + elem_size;
}

/* Returns the number of bytes the given array of strings will consume when
 * each string is appended as a separate element to a circular file. See
 * emer_circular_file_append for more details.
 */
static guint64
get_total_disk_size (const gchar * const *strings,
                     gsize                num_strings)
{
  guint64 total_disk_size = 0;
  for (gsize i = 0; i < num_strings; i++)
    total_disk_size += get_disk_size (strings[i]);

  return total_disk_size;
}

static EmerCircularFile *
make_circular_file (Fixture *fixture,
                    guint64  max_size)
{
  GError *error = NULL;
  EmerCircularFile *circular_file =
    emer_circular_file_new (fixture->data_file_path, max_size,
                            FALSE /* reinitialize */,
                            &error);

  g_assert_no_error (error);
  g_assert_nonnull (circular_file);

  return circular_file;
}

static EmerCircularFile *
make_minimal_circular_file (Fixture             *fixture,
                            const gchar * const *strings,
                            gsize                num_strings)
{
  guint64 max_size = get_total_disk_size (strings, num_strings);
  return make_circular_file (fixture, max_size);
}

static void
append_strings_and_check (EmerCircularFile    *circular_file,
                          const gchar * const *strings,
                          gsize                num_strings)
{
  for (gsize i = 0; i < num_strings; i++)
    {
      const gchar *curr_elem = strings[i];
      gsize elem_size = get_elem_size (curr_elem);
      emer_circular_file_append (circular_file, curr_elem, elem_size);
    }

  GError *error = NULL;
  gboolean save_succeeded = emer_circular_file_save (circular_file, &error);
  g_assert_no_error (error);
  g_assert_true (save_succeeded);
}

static guint64
read_strings_and_check (EmerCircularFile    *circular_file,
                        const gchar * const *strings,
                        gsize                num_strings)
{
  GBytes **elems;

  gsize total_elem_size = get_total_elem_size (strings, num_strings);
  gsize total_disk_size = get_total_disk_size (strings, num_strings);

  /* These arbitrary values should be overwritten. */
  gsize num_elems = num_strings + 1;
  guint64 token = total_disk_size + 1;

  GError *error = NULL;
  gboolean has_invalid;
  gboolean read_succeeded =
    emer_circular_file_read (circular_file, &elems, total_elem_size, &num_elems,
                             &token, &has_invalid, &error);

  g_assert_no_error (error);
  g_assert_true (read_succeeded);
  g_assert_cmpuint (num_elems, ==, num_strings);
  g_assert_false (has_invalid);

  for (gsize i = 0; i < num_elems; i++)
    {
      gsize actual_elem_size;
      gconstpointer elem_data = g_bytes_get_data (elems[i], &actual_elem_size);
      g_assert_cmpstr (elem_data, ==, strings[i]);
      gsize expected_elem_size = get_elem_size (strings[i]);
      g_assert_cmpuint (actual_elem_size, ==, expected_elem_size);
      g_bytes_unref (elems[i]);
    }

  g_free (elems);
  g_assert_cmpuint (token, ==, total_disk_size);

  return token;
}

static void
assert_circular_file_is_empty (EmerCircularFile *circular_file)
{
  g_assert_false (emer_circular_file_has_more (circular_file, 0));

  GBytes **elems;

  /* These arbitrary values should be overwritten. */
  gsize num_elems = 1;
  guint64 token = 1;

  GError *error = NULL;
  gboolean has_invalid;
  gboolean read_succeeded =
    emer_circular_file_read (circular_file, &elems, G_MAXSIZE, &num_elems,
                             &token, &has_invalid, &error);

  g_assert_no_error (error);
  g_assert_true (read_succeeded);
  g_assert_null (elems);
  g_assert_cmpuint (num_elems, ==, 0);
  g_assert_cmpuint (token, ==, 0);
  g_assert_false (has_invalid);
}

static void
remove_strings_and_check (EmerCircularFile    *circular_file,
                          const gchar * const *strings,
                          gsize                num_strings)
{
  guint64 token = read_strings_and_check (circular_file, strings, num_strings);
  GError *error = NULL;
  gboolean remove_succeeded =
    emer_circular_file_remove (circular_file, token, &error);

  g_assert_no_error (error);
  g_assert_true (remove_succeeded);
}

static void
purge_and_check_empty (EmerCircularFile *circular_file)
{
  GError *error = NULL;
  gboolean purge_succeeded = emer_circular_file_purge (circular_file, &error);

  g_assert_no_error (error);
  g_assert_true (purge_succeeded);
  assert_circular_file_is_empty (circular_file);
}

static void
test_circular_file_new (Fixture      *fixture,
                        gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 0);
  g_object_unref (circular_file);
}

static void
test_circular_file_append_when_empty (Fixture      *fixture,
                                      gconstpointer unused)
{
  const gchar *STRING = "Karl";
  guint64 max_size = get_disk_size (STRING);
  EmerCircularFile *circular_file = make_circular_file (fixture, max_size);

  gsize elem_size = get_elem_size (STRING);
  emer_circular_file_append (circular_file, STRING, elem_size);

  g_object_unref (circular_file);
}

static void
test_circular_file_append_when_full (Fixture      *fixture,
                                     gconstpointer unused)
{
  const gchar * const STRINGS[] =
    {
      "Sneezy", "phylum", "Europe", "sloth", "guacamole", "data link",
      "Colossus of Rhodes"
    };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  const gchar * const STRINGS_2[] = { "Marx" };
  gsize NUM_STRINGS_2 = G_N_ELEMENTS (STRINGS_2);
  gboolean will_fit =
    emer_circular_file_append (circular_file, STRINGS_2, NUM_STRINGS_2);
  g_assert_false (will_fit);

  g_object_unref (circular_file);
}

static void
test_circular_file_save_none (Fixture      *fixture,
                              gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 7823);

  append_strings_and_check (circular_file, NULL /* strings */, 0);

  g_object_unref (circular_file);
}

static void
test_circular_file_save_one (Fixture      *fixture,
                             gconstpointer unused)
{
  const gchar * const STRINGS[] = { "Gave" };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_save_many (Fixture      *fixture,
                              gconstpointer unused)
{
  const gchar * const STRINGS[] =
    {
      "Dopey", "class", "Australia", "wrath", "salsa", "physical",
      "Lighthouse of Alexandria"
    };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_read_none (Fixture      *fixture,
                              gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 0);

  append_strings_and_check (circular_file, NULL /* strings */, 0);
  read_strings_and_check (circular_file, NULL /* strings */, 0);

  g_object_unref (circular_file);
}

static void
test_circular_file_read_one (Fixture      *fixture,
                             gconstpointer unused)
{
  const gchar * const STRINGS[] = { "The" };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  read_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_read_many (Fixture      *fixture,
                              gconstpointer unused)
{
  const gchar * const STRINGS[] =
    {
      "Doc", "order", "Asia", "envy", "olives", "application",
      "Great Pyramid of Giza"
    };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  read_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_read_when_empty (Fixture      *fixture,
                                    gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 86);

  assert_circular_file_is_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_has_more (Fixture      *fixture,
                             gconstpointer unused)
{
  const gchar * const STRINGS[] =
    {
      "Grumpy", "family", "Africa", "pride", "refried beans", "presentation",
      "Hanging Gardens of Babylon"
    };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);

  gsize NUM_STRINGS_2 = (2 * NUM_STRINGS) - 1;
  const gchar *STRINGS_2[NUM_STRINGS_2];
  for (gsize i = 0; i < NUM_STRINGS_2; i++)
    STRINGS_2[i] = STRINGS[(i + 1) % NUM_STRINGS];

  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS_2, NUM_STRINGS_2);

  assert_circular_file_is_empty (circular_file);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  g_assert_true (emer_circular_file_has_more (circular_file, 0));

  guint64 token = read_strings_and_check (circular_file, STRINGS, NUM_STRINGS - 1);
  g_assert_true (emer_circular_file_has_more (circular_file, token));

  token = read_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  g_assert_false (emer_circular_file_has_more (circular_file, token));

  remove_strings_and_check (circular_file, STRINGS, 1);
  g_assert_true (emer_circular_file_has_more (circular_file, 0));

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  g_assert_true (emer_circular_file_has_more (circular_file, 0));

  token = read_strings_and_check (circular_file, STRINGS_2, NUM_STRINGS_2 - 1);
  g_assert_true (emer_circular_file_has_more (circular_file, token));

  token = read_strings_and_check (circular_file, STRINGS_2, NUM_STRINGS_2);
  g_assert_false (emer_circular_file_has_more (circular_file, token));

  remove_strings_and_check (circular_file, STRINGS_2, NUM_STRINGS_2);
  assert_circular_file_is_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_remove_none (Fixture      *fixture,
                                gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 50);

  append_strings_and_check (circular_file, NULL /* strings */, 0);
  remove_strings_and_check (circular_file, NULL /* strings */, 0);
  assert_circular_file_is_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_remove_one (Fixture      *fixture,
                               gconstpointer unused)
{
  const gchar * const STRINGS[] = { "Proletariat" };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  remove_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_remove_many (Fixture      *fixture,
                                gconstpointer unused)
{
  const gchar * const STRINGS[] =
    {
      "Happy", "genus", "North America", "lust", "cheese", "session",
      "Statue of Zeus at Olympia"
    };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  remove_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_remove_when_empty (Fixture      *fixture,
                                      gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 0);

  remove_strings_and_check (circular_file, NULL /* strings */, 0);
  assert_circular_file_is_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_purge_none (Fixture      *fixture,
                               gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 33);

  append_strings_and_check (circular_file, NULL /* strings */, 0);
  purge_and_check_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_purge_one (Fixture      *fixture,
                              gconstpointer unused)
{
  const gchar * const STRINGS[] = { "Eleven" };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  purge_and_check_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_purge_many (Fixture      *fixture,
                               gconstpointer unused)
{
  const gchar * const STRINGS[] =
    {
      "Sleepy", "species", "South America", "gluttony", "ground beef",
      "transport", "Temple of Artemis at Ephesus"
    };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  purge_and_check_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_purge_when_empty (Fixture      *fixture,
                                     gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 33);

  purge_and_check_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_ignores_unsaved_elems (Fixture      *fixture,
                                          gconstpointer unused)
{
  const gchar * const STRINGS[] = { "Zeppelins" };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  EmerCircularFile *circular_file =
    make_minimal_circular_file (fixture, STRINGS, NUM_STRINGS);

  gsize elem_size = get_elem_size (STRINGS[0]);
  emer_circular_file_append (circular_file, STRINGS[0], elem_size);
  assert_circular_file_is_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_grow (Fixture      *fixture,
                         gconstpointer unused)
{
  const gchar * const STRINGS[] = { "Yo" };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  gsize max_size = get_total_disk_size (STRINGS, NUM_STRINGS);
  EmerCircularFile *circular_file = make_circular_file (fixture, max_size);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  remove_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);

  gsize max_size_2 = 2 * max_size;
  EmerCircularFile *circular_file_2 = make_circular_file (fixture, max_size_2);

  append_strings_and_check (circular_file_2, STRINGS, NUM_STRINGS);
  remove_strings_and_check (circular_file_2, STRINGS, NUM_STRINGS);
  remove_strings_and_check (circular_file_2, STRINGS, NUM_STRINGS);
  assert_circular_file_is_empty (circular_file_2);

  g_object_unref (circular_file_2);
}

static void
test_circular_file_shrink (Fixture      *fixture,
                           gconstpointer unused)
{
  const gchar * const STRINGS[] =
    {
      "Bashful", "kingdom", "Antarctica", "greed", "sour cream", "network",
      "Mausoleum at Halicarnassus"
    };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  gsize max_size = get_total_disk_size (STRINGS, NUM_STRINGS);
  EmerCircularFile *circular_file = make_circular_file (fixture, max_size);

  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  remove_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);

  gsize max_size_2 = max_size - 1;
  EmerCircularFile *circular_file_2 = make_circular_file (fixture, max_size_2);

  remove_strings_and_check (circular_file_2, STRINGS, NUM_STRINGS - 1);

  g_object_unref (circular_file_2);
}

static void
assert_circular_file_works_after_recovery (Fixture          *fixture,
                                           EmerCircularFile *circular_file,
                                           const gsize       max_size)
{
  /* Verify that the file is logically empty, even though it's physically full
   * of asterisks.
   */
  const gchar * const NO_STRINGS[0];
  guint64 token = read_strings_and_check (circular_file, NO_STRINGS, 0);
  g_assert_false (emer_circular_file_has_more (circular_file, token));

  /* Adding new entries to the file and reading them back should work. */
  const gchar * const STRINGS[] = { "Kendal Mint Cake" };
  gsize NUM_STRINGS = G_N_ELEMENTS (STRINGS);
  append_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
  read_strings_and_check (circular_file, STRINGS, NUM_STRINGS);

  /* Reloading the file should work, and the entry we just added should be
   * readable.
   */
  g_autoptr(GError) error = NULL;
  g_autoptr(EmerCircularFile) reloaded =
    emer_circular_file_new (fixture->data_file_path, max_size,
                            FALSE /* reinitialize */,
                            &error);
  g_assert_no_error (error);
  g_assert_nonnull (circular_file);

  read_strings_and_check (circular_file, STRINGS, NUM_STRINGS);
}

/* Helper for test cases where the metadata file exists but has empty contents,
 * morally equivalent to not existing at all.
 */
static void
test_circular_file_emptyish_metadata_file (Fixture      *fixture,
                                           const gchar  *metadata_file_contents,
                                           gssize        metadata_file_length)
{
  const gsize max_size = 1024;
  g_autofree gchar *data_file_contents = g_malloc (max_size);
  g_autofree gchar *metadata_file_path =
    g_strconcat (fixture->data_file_path, METADATA_EXTENSION, NULL);
  g_autoptr(GError) error = NULL;
  gboolean ret;

  /* Fill the data file with junk. */
  memset (data_file_contents, '*', max_size);
  ret = g_file_set_contents (fixture->data_file_path, data_file_contents,
                             max_size, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  /* Initialize the metadata file to the desired incorrect contents */
  ret = g_file_set_contents (metadata_file_path, metadata_file_contents,
                             metadata_file_length, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  /* The file should load successfully */
  g_autoptr(EmerCircularFile) circular_file =
    emer_circular_file_new (fixture->data_file_path, max_size,
                            FALSE /* reinitialize */,
                            &error);
  g_assert_no_error (error);
  g_assert_nonnull (circular_file);

  assert_circular_file_works_after_recovery (fixture, circular_file, max_size);
}

static void
test_circular_file_metadata_file_nul_bytes (Fixture      *fixture,
                                            gconstpointer unused)
{
  gssize length = 43;
  g_autofree gchar *contents = g_malloc0 (length);

  /* To summarize this bug: we observed a metadata file containing 43 NUL
   * bytes. The initial metadata keyfile, if properly written, is 43 bytes
   * long. This happens because g_file_set_contents() followed by a system
   * crash can leave the target file in a state where the file has been
   * allocated but its contents not committed, if and only if the file didn't
   * previously exist or was empty.
   *
   * https://bugzilla.gnome.org/show_bug.cgi?id=790638
   *
   * A reasonable recovery path is to re-initialize the metadata, assuming no
   * events had previously been stored. This is a safe assumption because if
   * events *had* been written to the file, g_file_set_contents() would have
   * been called a second time; this time, the target file would have already
   * been non-empty, so g_file_set_contents() would have fsynced() the
   * temporary file before rename(), which provides the expected "old or new"
   * guarantee after a crash.
   */
  g_test_bug ("T19953");

  test_circular_file_emptyish_metadata_file (fixture, contents, length);
}

static void
test_circular_file_metadata_file_empty (Fixture      *fixture,
                                        gconstpointer unused)
{
  /* If the metadata file exists but is empty, we should initialize it and
   * consider the circular file itself to be empty. In particular, if glib is
   * compiled without fallocate() or the call within g_file_set_contents()
   * fails, this is what we'd observe in the crash-after-first-write case.
   */
  test_circular_file_emptyish_metadata_file (fixture, "", 0);
}

/* Helper for tests where the metadata file is actively malformed, not just
 * morally empty. In these cases it is not safe to assume that there was no
 * previous data in the circular file. While the surrounding daemon will want
 * to recover by reinitializing the circular file, it needs to be able to
 * detect this case so we can report a "circular file corrupt" event.
 */
static void
test_circular_file_broken_metadata_file (Fixture      *fixture,
                                         const gchar  *metadata_file_contents,
                                         gssize        metadata_file_length,
                                         GKeyFileError expected_error_code)
{
  const gsize max_size = 1024;
  g_autofree gchar *data_file_contents = g_malloc (max_size);
  g_autofree gchar *metadata_file_path =
    g_strconcat (fixture->data_file_path, METADATA_EXTENSION, NULL);
  g_autoptr(GError) error = NULL;
  gboolean ret;

  /* Fill the data file with junk. */
  memset (data_file_contents, '*', max_size);
  ret = g_file_set_contents (fixture->data_file_path, data_file_contents,
                             max_size, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  /* Initialize the metadata file to the desired incorrect contents */
  ret = g_file_set_contents (metadata_file_path, metadata_file_contents,
                             metadata_file_length, &error);
  g_assert_no_error (error);
  g_assert_true (ret);

  g_autoptr(EmerCircularFile) circular_file = NULL;

  /* Attempting to load the file with reinitialize=FALSE should fail, with the
   * given error code.
   */
  circular_file = emer_circular_file_new (fixture->data_file_path, max_size,
                                          FALSE /* reinitialize */,
                                          &error);
  g_assert_null (circular_file);
  g_assert_error (error, G_KEY_FILE_ERROR, expected_error_code);
  g_clear_error (&error);

  /* Re-initializing the file should work, though. */
  circular_file = emer_circular_file_new (fixture->data_file_path, max_size,
                                          TRUE /* reinitialize */,
                                          &error);
  g_assert_no_error (error);
  g_assert_nonnull (circular_file);

  assert_circular_file_works_after_recovery (fixture, circular_file, max_size);
}

static void
test_circular_file_metadata_file_junk (Fixture      *fixture,
                                       gconstpointer unused)
{
  gssize length = 43;
  gchar contents[length];

  /* Totally broken! */
  memset (contents, '!', length);

  test_circular_file_broken_metadata_file (fixture, contents, length,
                                           G_KEY_FILE_ERROR_PARSE);
}

static void
test_circular_file_metadata_file_missing_max_size (Fixture      *fixture,
                                                   gconstpointer unused)
{
  const gchar contents[] = "[metadata]\nsize=1024\nhead=0\n";
  const gssize length = sizeof contents;

  /* max_size is missing; just assume the file is empty, since this will never
   * happen in practice without other keys also being missing unless someone
   * edits the file by hand and then you get to keep both pieces.
   */
  test_circular_file_broken_metadata_file (fixture, contents, length,
                                           G_KEY_FILE_ERROR_KEY_NOT_FOUND);
}

static void
test_circular_file_metadata_file_missing_size (Fixture      *fixture,
                                               gconstpointer unused)
{
  const gchar contents[] = "[metadata]\nmax_size=1024\nhead=27\n";
  const gssize length = sizeof contents;

  /* size is missing; we should recover by treating the file as empty because
   * we do not know at what byte we should loop around.
   */
  test_circular_file_broken_metadata_file (fixture, contents, length,
                                           G_KEY_FILE_ERROR_KEY_NOT_FOUND);
}

static void
test_circular_file_metadata_file_missing_head (Fixture      *fixture,
                                               gconstpointer unused)
{
  const gchar contents[] = "[metadata]\nmax_size=1024\nsize=1024\n";
  const gssize length = sizeof contents;

  /* size is missing; we should recover by treating the file as empty, because
   * we don't know where the head was supposed to be.
   */
  test_circular_file_broken_metadata_file (fixture, contents, length,
                                           G_KEY_FILE_ERROR_KEY_NOT_FOUND);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);
  g_test_bug_base ("https://phabricator.endlessm.com/");

#define ADD_CIRCULAR_FILE_TEST_FUNC(path, func) \
  g_test_add((path), Fixture, NULL, setup, (func), teardown)

  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/new", test_circular_file_new);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/append-when-empty",
                               test_circular_file_append_when_empty);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/append-when-full",
                               test_circular_file_append_when_full);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/save-none",
                               test_circular_file_save_none);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/save-one",
                               test_circular_file_save_one);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/save-many",
                               test_circular_file_save_many);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/read-none",
                               test_circular_file_read_none);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/read-one",
                               test_circular_file_read_one);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/read-many",
                               test_circular_file_read_many);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/read-when-empty",
                               test_circular_file_read_when_empty);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/has-more",
                               test_circular_file_has_more);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/remove-none",
                               test_circular_file_remove_none);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/remove-one",
                               test_circular_file_remove_one);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/remove-many",
                               test_circular_file_remove_many);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/remove-when-empty",
                               test_circular_file_remove_when_empty);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/purge-none",
                               test_circular_file_purge_none);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/purge-one",
                               test_circular_file_purge_one);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/purge-many",
                               test_circular_file_purge_many);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/purge-when-empty",
                               test_circular_file_purge_when_empty);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/ignores-unsaved-elems",
                               test_circular_file_ignores_unsaved_elems);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/grow", test_circular_file_grow);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/shrink",
                               test_circular_file_shrink);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/metadata-file-nul-bytes",
                               test_circular_file_metadata_file_nul_bytes);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/metadata-file-empty",
                               test_circular_file_metadata_file_empty);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/metadata-file-junk",
                               test_circular_file_metadata_file_junk);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/metadata-file-missing-max_size",
                               test_circular_file_metadata_file_missing_max_size);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/metadata-file-missing-size",
                               test_circular_file_metadata_file_missing_size);
  ADD_CIRCULAR_FILE_TEST_FUNC ("/circular-file/metadata-file-missing-head",
                               test_circular_file_metadata_file_missing_head);
#undef ADD_CIRCULAR_FILE_TEST_FUNC

  return g_test_run ();
}
