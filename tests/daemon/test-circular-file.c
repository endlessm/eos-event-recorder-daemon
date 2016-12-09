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
    emer_circular_file_new (fixture->data_file_path, max_size, &error);

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
assert_strings_saved (EmerCircularFile    *circular_file,
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
assert_strings_read (EmerCircularFile    *circular_file,
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
assert_strings_removed (EmerCircularFile    *circular_file,
                        const gchar * const *strings,
                        gsize                num_strings)
{
  guint64 token = assert_strings_read (circular_file, strings, num_strings);
  GError *error = NULL;
  gboolean remove_succeeded =
    emer_circular_file_remove (circular_file, token, &error);

  g_assert_no_error (error);
  g_assert_true (remove_succeeded);
}

static void
assert_circular_file_purged (EmerCircularFile *circular_file)
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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);

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

  assert_strings_saved (circular_file, NULL /* strings */, 0);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_read_none (Fixture      *fixture,
                              gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 0);

  assert_strings_saved (circular_file, NULL /* strings */, 0);
  assert_strings_read (circular_file, NULL /* strings */, 0);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_read (circular_file, STRINGS, NUM_STRINGS);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_read (circular_file, STRINGS, NUM_STRINGS);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  g_assert_true (emer_circular_file_has_more (circular_file, 0));

  guint64 token = assert_strings_read (circular_file, STRINGS, NUM_STRINGS - 1);
  g_assert_true (emer_circular_file_has_more (circular_file, token));

  token = assert_strings_read (circular_file, STRINGS, NUM_STRINGS);
  g_assert_false (emer_circular_file_has_more (circular_file, token));

  assert_strings_removed (circular_file, STRINGS, 1);
  g_assert_true (emer_circular_file_has_more (circular_file, 0));

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  g_assert_true (emer_circular_file_has_more (circular_file, 0));

  token = assert_strings_read (circular_file, STRINGS_2, NUM_STRINGS_2 - 1);
  g_assert_true (emer_circular_file_has_more (circular_file, token));

  token = assert_strings_read (circular_file, STRINGS_2, NUM_STRINGS_2);
  g_assert_false (emer_circular_file_has_more (circular_file, token));

  assert_strings_removed (circular_file, STRINGS_2, NUM_STRINGS_2);
  assert_circular_file_is_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_remove_none (Fixture      *fixture,
                                gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 50);

  assert_strings_saved (circular_file, NULL /* strings */, 0);
  assert_strings_removed (circular_file, NULL /* strings */, 0);
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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_removed (circular_file, STRINGS, NUM_STRINGS);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_removed (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);
}

static void
test_circular_file_remove_when_empty (Fixture      *fixture,
                                      gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 0);

  assert_strings_removed (circular_file, NULL /* strings */, 0);
  assert_circular_file_is_empty (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_purge_none (Fixture      *fixture,
                               gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 33);

  assert_strings_saved (circular_file, NULL /* strings */, 0);
  assert_circular_file_purged (circular_file);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_circular_file_purged (circular_file);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_circular_file_purged (circular_file);

  g_object_unref (circular_file);
}

static void
test_circular_file_purge_when_empty (Fixture      *fixture,
                                     gconstpointer unused)
{
  EmerCircularFile *circular_file = make_circular_file (fixture, 33);

  assert_circular_file_purged (circular_file);

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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_removed (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);

  gsize max_size_2 = 2 * max_size;
  EmerCircularFile *circular_file_2 = make_circular_file (fixture, max_size_2);

  assert_strings_saved (circular_file_2, STRINGS, NUM_STRINGS);
  assert_strings_removed (circular_file_2, STRINGS, NUM_STRINGS);
  assert_strings_removed (circular_file_2, STRINGS, NUM_STRINGS);
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

  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_removed (circular_file, STRINGS, NUM_STRINGS);
  assert_strings_saved (circular_file, STRINGS, NUM_STRINGS);

  g_object_unref (circular_file);

  gsize max_size_2 = max_size - 1;
  EmerCircularFile *circular_file_2 = make_circular_file (fixture, max_size_2);

  assert_strings_removed (circular_file_2, STRINGS, NUM_STRINGS - 1);

  g_object_unref (circular_file_2);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);

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
#undef ADD_CIRCULAR_FILE_TEST_FUNC

  return g_test_run ();
}
