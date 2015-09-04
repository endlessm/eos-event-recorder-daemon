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

#include "emer-persistent-cache.h"

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <eosmetrics/eosmetrics.h>

#include "emer-boot-id-provider.h"
#include "emer-cache-size-provider.h"
#include "emer-cache-version-provider.h"
#include "shared/metrics-util.h"

#define TEST_DIRECTORY "/tmp/metrics_testing/"

#define TEST_CACHE_SIZE_FILE "cache_size_file"
#define TEST_SYSTEM_BOOT_ID_FILE "system_boot_id_file"
#define TEST_CACHE_VERSION_FILE "local_version_file"

// Generated via uuidgen.
#define FAKE_SYSTEM_BOOT_ID "1ca14ab8-bed6-4bc0-8369-484518d22a31\n"
#define FAKE_BOOT_ID "baccd4dd-9765-4eb2-a2a0-03c6623471e6\n"
#define FAKE_BOOT_OFFSET 4000000000 // 4 seconds

#define TEST_UPDATE_OFFSET_INTERVAL (60u * 60u) // 1 hour

/*
 * The expected size in bytes of the boot id file we want to mock, located at
 * /proc/sys/kernel/random/boot_id. The file should be 32 lower-case hexadecimal
 * characters interspersed with 4 hyphens and terminated with a newline
 * character.
 *
 * Exact format: "%08x-%04x-%04x-%04x-%012x\n"
 */
#define BOOT_FILE_LENGTH 37

#define ACCEPTABLE_OFFSET_VARIANCE 500000000 // 500 milliseconds

#define CACHE_SIZE_KEY_FILE_DATA \
  "[persistent_cache_size]\n" \
  "maximum=10000000\n"

#define DEFAULT_CACHE_VERSION_KEY_FILE_DATA \
  "[cache_version_info]\n" \
  "version=4\n"

// ---- Helper functions come first ----

static void
write_cache_size_file (void)
{
  GKeyFile *key_file = g_key_file_new ();
  GError *error = NULL;
  g_key_file_load_from_data (key_file, CACHE_SIZE_KEY_FILE_DATA, -1,
                             G_KEY_FILE_NONE, &error);
  g_assert_no_error (error);

  g_key_file_save_to_file (key_file, TEST_DIRECTORY TEST_CACHE_SIZE_FILE,
                           &error);
  g_assert_no_error (error);

  g_key_file_unref (key_file);
}

static void
write_mock_system_boot_id_file (void)
{
  GFile *file = g_file_new_for_path (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  GError *error = NULL;
  g_file_replace_contents (file, FAKE_SYSTEM_BOOT_ID, BOOT_FILE_LENGTH, NULL,
                           FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                           NULL, &error);
  g_assert_no_error (error);
  g_object_unref (file);
}

static void
write_default_cache_version_key_file (void)
{
  GKeyFile *key_file = g_key_file_new ();
  gboolean load_succeeded =
    g_key_file_load_from_data (key_file, DEFAULT_CACHE_VERSION_KEY_FILE_DATA,
                               -1, G_KEY_FILE_NONE, NULL);
  g_assert_true (load_succeeded);

  gboolean save_succeeded =
    g_key_file_save_to_file (key_file, TEST_DIRECTORY TEST_CACHE_VERSION_FILE, NULL);
  g_assert_true (save_succeeded);
  g_key_file_unref (key_file);
}

static void
teardown (gboolean     *unused,
          gconstpointer dontuseme)
{
  g_unlink (TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE);
  g_unlink (TEST_DIRECTORY TEST_CACHE_SIZE_FILE);
  g_unlink (TEST_DIRECTORY TEST_CACHE_VERSION_FILE);
  g_unlink (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  g_rmdir (TEST_DIRECTORY);
}

static void
setup (gboolean     *unused,
       gconstpointer dontuseme)
{
  teardown (unused, dontuseme);
  g_mkdir (TEST_DIRECTORY, 0777); // All permissions are granted by 0777.
  write_cache_size_file ();
  write_mock_system_boot_id_file ();
  write_default_cache_version_key_file ();
}

static EmerPersistentCache *
make_testing_cache (void)
{
  EmerCacheSizeProvider *cache_size_provider =
    emer_cache_size_provider_new_full (TEST_DIRECTORY TEST_CACHE_SIZE_FILE);
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new (NULL);
  GError *error = NULL;
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_individual.metrics. Error: *.");
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_aggregate.metrics. Error: *.");
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_sequence.metrics. Error: *.");
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (TEST_DIRECTORY, cache_size_provider,
                                    boot_id_provider, cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL, &error);
  g_test_assert_expected_messages ();
  g_assert_no_error (error);
  g_assert_nonnull (cache);

  g_object_unref (cache_size_provider);
  g_object_unref (boot_id_provider);
  g_object_unref (cache_version_provider);

  return cache;
}

/*
 * Returns a new key file associated with the boot timing metadata file.
 * Keyfile should be unref'd via g_key_file_unref().
 */
static GKeyFile *
load_boot_offset_key_file (void)
{
  GKeyFile *boot_offset_key_file = g_key_file_new ();
  gboolean load_succeeded =
    g_key_file_load_from_file (boot_offset_key_file,
                               TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE,
                               G_KEY_FILE_NONE,
                               NULL);
  g_assert_true (load_succeeded);
  return boot_offset_key_file;
}

/*
 * Will overwrite the contents of the boot id metadata file's boot offset with a
 * given new_offset.
 */
static void
set_boot_offset_in_metadata_file (gint64 new_offset)
{
  GKeyFile *boot_offset_key_file = load_boot_offset_key_file ();

  g_key_file_set_int64 (boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                        CACHE_BOOT_OFFSET_KEY, new_offset);

  gboolean save_succeeded =
    g_key_file_save_to_file (boot_offset_key_file,
                             TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE, NULL);
  g_assert_true (save_succeeded);
  g_key_file_unref (boot_offset_key_file);
}

/*
 * Overwrites the metadata file's boot id with the given boot id.
 */
static void
set_boot_id_in_metadata_file (gchar *boot_id)
{
  GKeyFile *boot_offset_key_file = load_boot_offset_key_file ();

  g_key_file_set_string (boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                         CACHE_LAST_BOOT_ID_KEY, boot_id);

  gboolean save_succeeded =
    g_key_file_save_to_file (boot_offset_key_file,
                             TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE, NULL);
  g_assert_true (save_succeeded);
  g_key_file_unref (boot_offset_key_file);
}

/*
 * Gets the stored offset from the metadata file.
 */
static gint64
read_offset (void)
{
  GKeyFile *boot_offset_key_file = load_boot_offset_key_file ();
  GError *error = NULL;
  gint64 stored_offset =
    g_key_file_get_int64 (boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_BOOT_OFFSET_KEY, &error);
  g_key_file_unref (boot_offset_key_file);
  g_assert_no_error (error);
  return stored_offset;
}

/*
 * Gets the metadata file was reset flag from the metadata file.
 * Also ensures the reset value is 0.
 */
static gboolean
boot_offset_was_reset (void)
{
  GKeyFile *boot_offset_key_file = load_boot_offset_key_file ();
  GError *error = NULL;
  gboolean was_reset =
    g_key_file_get_boolean (boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                            CACHE_WAS_RESET_KEY, &error);
  g_key_file_unref (boot_offset_key_file);
  g_assert_no_error (error);
  return was_reset && (read_offset() == 0);
}

/*
 * Gets the stored relative time from the metadata file.
 */
static gint64
read_relative_time (void)
{
  GKeyFile *boot_offset_key_file = load_boot_offset_key_file ();
  GError *error = NULL;
  gint64 stored_offset =
    g_key_file_get_int64 (boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_RELATIVE_TIME_KEY, &error);
  g_key_file_unref (boot_offset_key_file);
  g_assert_no_error (error);
  return stored_offset;
}

/*
 * Gets the stored absolute time from the metadata file.
 */
static gint64
read_absolute_time (void)
{
  GKeyFile *boot_offset_key_file = load_boot_offset_key_file ();
  GError *error = NULL;
  gint64 stored_offset =
    g_key_file_get_int64 (boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_ABSOLUTE_TIME_KEY, &error);
  g_key_file_unref (boot_offset_key_file);
  g_assert_no_error (error);
  return stored_offset;
}

/*
 * Reads the timestamps in the metadata file and returns TRUE if they are
 * greater than or equal to the given timestamps and less than or equal to
 * subsequently generated timestamps. Returns FALSE otherwise.
 */
static gboolean
boot_timestamp_is_valid (gint64 previous_relative_time,
                         gint64 previous_absolute_time)
{
  gint64 stored_relative_time = read_relative_time ();
  gint64 stored_absolute_time = read_absolute_time ();

  gint64 after_relative_time;
  gboolean get_succeeded =
    emtr_util_get_current_time (CLOCK_BOOTTIME, &after_relative_time);
  g_assert_true (get_succeeded);

  gint64 after_absolute_time;
  get_succeeded =
    emtr_util_get_current_time (CLOCK_REALTIME, &after_absolute_time);
  g_assert_true (get_succeeded);

  // The actual testing:
  return (previous_relative_time <= stored_relative_time &&
          stored_relative_time   <= after_relative_time  &&
          previous_absolute_time <= stored_absolute_time &&
          stored_absolute_time   <= after_absolute_time);
}

static GVariant *
make_variant (gint choice)
{
  switch (choice)
    {
    case 0:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (1876),
                            g_variant_new_double (3.14159));

    case 1:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (0),
                            g_variant_new_string ("minus-1-steve"));

    case 2:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (-1), NULL);

    case 3:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (7),
                            g_variant_new_double (2.71828));

    case 4:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (67352),
                            g_variant_new_string ("Help!"));

    case 5:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (747), NULL);

    case 6:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (57721),
                            g_variant_new_string ("Secret"));

    case 7:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (-100),
                            g_variant_new_double (120.20569));

    case 8:
      return g_variant_new ("(xmv)", G_GINT64_CONSTANT (127384),
                            g_variant_new_double (-2.685452));

    case 9:
      return g_variant_new ("(uxmv)", 234u, G_GINT64_CONSTANT (42),
                            g_variant_new_string ("murphy"));

    case 10:
      return g_variant_new ("(uxmv)", 121u, G_GINT64_CONSTANT (999),
                            g_variant_new_int32 (404));

    case 11:
      return g_variant_new ("(uxmv)", 555u, G_GINT64_CONSTANT (1201),
                            g_variant_new_string ("baa!"));

    case 12:
      return g_variant_new ("(uxmv)", 411u, G_GINT64_CONSTANT (-128),
                            g_variant_new_int32 (64));

    case 13:
      return g_variant_new ("(uxxmv)", 12u, G_GINT64_CONSTANT (111),
                            G_GINT64_CONSTANT (9876),
                            g_variant_new_string ("meepo"));

    case 14:
      return g_variant_new ("(uxxmv)", 1019u, G_GINT64_CONSTANT (1),
                            G_GINT64_CONSTANT (-333),
                            g_variant_new_string ("la la la"));

    case 15:
      return g_variant_new ("(uxxmv)", 5u, G_GINT64_CONSTANT (-3984),
                            G_GINT64_CONSTANT (5965),
                            g_variant_new_string ("Gandalf"));

    default:
      g_error ("Tried to make a variant that hasn't been programmed.");
    }
}

static GPtrArray *
make_many_variants (void)
{
  gsize NUM_VARIANTS = 16;
  GPtrArray *variants =
    g_ptr_array_new_full (NUM_VARIANTS, (GDestroyNotify) g_variant_unref);
  for (gsize i = 0; i < NUM_VARIANTS; i++)
    {
      GVariant *curr_variant = make_variant (i);
      g_variant_ref_sink (curr_variant);
      g_ptr_array_add (variants, curr_variant);
    }

  return variants;
}

/* Returns the number of bytes the given variant will consume when stored in a
 * persistent cache. See emer_circular_file_append for more details.
 */
static guint64
get_size_when_stored (GVariant *variant)
{
  gsize cost = emer_persistent_cache_cost (variant);
  return sizeof (guint64) + cost;
}

static void
assert_variants_equal (GVariant **actual_variants,
                       GVariant **expected_variants,
                       gsize      num_variants)
{
  if (num_variants == 0)
    {
      g_assert_null (actual_variants);
      return;
    }

  for (gsize i = 0; i < num_variants; i++)
    g_assert_true (g_variant_equal (actual_variants[i], expected_variants[i]));
}

static void
assert_variants_stored (EmerPersistentCache *cache,
                        GVariant           **variants,
                        gsize                num_variants)
{
  /* This arbitrary value should be overwritten. */
  gsize num_variants_stored = num_variants + 1;

  GError *error = NULL;
  gboolean store_succeeded =
    emer_persistent_cache_store (cache, variants, num_variants,
                                 &num_variants_stored, &error);

  g_assert_no_error (error);
  g_assert_true (store_succeeded);
  g_assert_cmpuint (num_variants_stored, ==, num_variants);
}

static guint64
assert_variants_read (EmerPersistentCache *cache,
                      GVariant           **variants,
                      gsize                num_variants)
{
  GVariant **variants_read;

  gsize total_elem_size = 0;
  gsize total_size_when_stored = 0;
  for (gsize i = 0; i < num_variants; i++)
    {
      total_elem_size += emer_persistent_cache_cost (variants[i]);
      total_size_when_stored += get_size_when_stored (variants[i]);
    }

  /* These arbitrary values should be overwritten. */
  gsize num_variants_read = num_variants + 1;
  guint64 token = total_size_when_stored + 1;

  GError *error = NULL;
  gboolean read_succeeded =
    emer_persistent_cache_read (cache, &variants_read, total_elem_size,
                                &num_variants_read, &token, &error);

  g_assert_no_error (error);
  g_assert_true (read_succeeded);
  g_assert_cmpuint (num_variants_read, ==, num_variants);
  assert_variants_equal (variants_read, variants, num_variants);
  g_assert_cmpuint (token, ==, total_size_when_stored);

  destroy_variants (variants_read, num_variants);

  return token;
}

static void
assert_cache_is_empty (EmerPersistentCache *cache)
{
  g_assert_false (emer_persistent_cache_has_more (cache, 0));

  GVariant **variants;

  /* These arbitrary values should be overwritten. */
  gsize num_variants = 1;
  guint64 token = 1;

  GError *error = NULL;
  gboolean read_succeeded =
    emer_persistent_cache_read (cache, &variants, G_MAXSIZE, &num_variants,
                                &token, &error);

  g_assert_no_error (error);
  g_assert_true (read_succeeded);
  g_assert_null (variants);
  g_assert_cmpuint (num_variants, ==, 0);
  g_assert_cmpuint (token, ==, 0);
}

static void
assert_variants_removed (EmerPersistentCache *cache,
                         GVariant           **variants,
                         gsize                num_variants)
{
  guint64 token = assert_variants_read (cache, variants, num_variants);
  GError *error = NULL;
  gboolean remove_succeeded =
    emer_persistent_cache_remove (cache, token, &error);

  g_assert_no_error (error);
  g_assert_true (remove_succeeded);
}

/* ----- Actual Test Cases below ------ */

static void
test_persistent_cache_new (gboolean     *unused,
                           gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  g_object_unref (cache);
}

static void
test_persistent_cache_cost (gboolean     *unused,
                            gconstpointer dontuseme)
{
  GPtrArray *variants = make_many_variants ();
  for (gsize i = 0; i < variants->len; i++)
    {
      GVariant *curr_variant = g_ptr_array_index (variants, i);
      gsize actual_cost = emer_persistent_cache_cost (curr_variant);
      const gchar *type_string = g_variant_get_type_string (curr_variant);
      gsize type_string_length = strlen (type_string) + 1;
      gsize variant_size = g_variant_get_size (curr_variant);
      gsize expected_cost = type_string_length + variant_size;
      g_assert_cmpuint (actual_cost, ==, expected_cost);
    }

  g_ptr_array_unref (variants);
}

static void
test_persistent_cache_store_none (gboolean     *unused,
                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  assert_variants_stored (cache, NULL /* variants */, 0);

  g_object_unref (cache);
}

static void
test_persistent_cache_store_one (gboolean     *unused,
                                 gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  GVariant *variants[] = { make_variant (0) };
  gsize num_variants = G_N_ELEMENTS (variants);
  assert_variants_stored (cache, variants, num_variants);

  g_object_unref (cache);
}

static void
test_persistent_cache_store_many (gboolean     *unused,
                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  GPtrArray *variants = make_many_variants ();
  assert_variants_stored (cache, (GVariant **) variants->pdata, variants->len);
  g_ptr_array_unref (variants);

  g_object_unref (cache);
}

static void
test_persistent_cache_store_when_full (gboolean     *unused,
                                       gconstpointer dontuseme)
{
  EmerCacheSizeProvider *cache_size_provider =
    emer_cache_size_provider_new_full (TEST_DIRECTORY TEST_CACHE_SIZE_FILE);
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new (NULL);
  GError *error = NULL;
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_individual.metrics. Error: *.");
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_aggregate.metrics. Error: *.");
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_sequence.metrics. Error: *.");
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (TEST_DIRECTORY, cache_size_provider,
                                    boot_id_provider, cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL, &error);
  g_test_assert_expected_messages ();
  g_assert_no_error (error);
  g_assert_nonnull (cache);

  g_object_unref (boot_id_provider);
  g_object_unref (cache_version_provider);

  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (cache_size_provider);
  g_object_unref (cache_size_provider);

  GVariant *variant = make_variant (0);
  g_variant_ref_sink (variant);
  gsize num_variants = max_cache_size / get_size_when_stored (variant);
  GVariant *variants[num_variants];
  for (gsize i = 0; i < num_variants; i++)
    variants[i] = variant;

  assert_variants_stored (cache, variants, num_variants);

  /* This arbitrary value should be overwritten. */
  gsize num_variants_stored = 1;

  gboolean store_succeeded =
    emer_persistent_cache_store (cache, &variant, 1, &num_variants_stored,
                                 &error);

  g_assert_no_error (error);
  g_assert_true (store_succeeded);
  g_assert_cmpuint (num_variants_stored, ==, 0);

  g_variant_unref (variant);
  g_object_unref (cache);
}

static void
test_persistent_cache_read_none (gboolean     *unused,
                                 gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  assert_variants_stored (cache, NULL /* variants */, 0);
  assert_variants_read (cache, NULL /* variants */, 0);

  g_object_unref (cache);
}

static void
test_persistent_cache_read_one (gboolean     *unused,
                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  GVariant *variant = make_variant (0);
  g_variant_ref_sink (variant);
  GVariant *variants[] = { variant };
  gsize num_variants = G_N_ELEMENTS (variants);
  assert_variants_stored (cache, variants, num_variants);
  assert_variants_read (cache, variants, num_variants);

  g_variant_unref (variant);
  g_object_unref (cache);
}

static void
test_persistent_cache_read_many (gboolean     *unused,
                                 gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  GPtrArray *variants = make_many_variants ();

  assert_variants_stored (cache, (GVariant **) variants->pdata, variants->len);
  assert_variants_read (cache, (GVariant **) variants->pdata, variants->len);

  g_ptr_array_unref (variants);
  g_object_unref (cache);
}

static void
test_persistent_cache_read_when_empty (gboolean     *unused,
                                       gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  assert_cache_is_empty (cache);
  g_object_unref (cache);
}

static void
test_persistent_cache_has_more (gboolean     *unused,
                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  assert_cache_is_empty (cache);

  GPtrArray *variants = make_many_variants ();

  assert_variants_stored (cache, (GVariant **) variants->pdata, variants->len);
  g_assert_true (emer_persistent_cache_has_more (cache, 0));

  guint64 token =
    assert_variants_read (cache, (GVariant **) variants->pdata,
                          variants->len - 1);
  g_assert_true (emer_persistent_cache_has_more (cache, token));

  token =
    assert_variants_read (cache, (GVariant **) variants->pdata, variants->len);
  g_assert_false (emer_persistent_cache_has_more (cache, token));

  assert_variants_removed (cache, (GVariant **) variants->pdata, 1);
  g_assert_true (emer_persistent_cache_has_more (cache, 0));

  assert_variants_stored (cache, (GVariant **) variants->pdata, variants->len);
  g_assert_true (emer_persistent_cache_has_more (cache, 0));

  gsize curr_length = variants->len;
  for (gsize i = 0; i < curr_length; i++)
    {
      GVariant *curr_variant = g_ptr_array_index (variants, i);
      g_variant_ref (curr_variant);
      g_ptr_array_add (variants, curr_variant);
    }

  g_ptr_array_remove_index (variants, 0);

  token =
    assert_variants_read (cache, (GVariant **) variants->pdata,
                          variants->len - 1);
  g_assert_true (emer_persistent_cache_has_more (cache, token));

  token =
    assert_variants_read (cache, (GVariant **) variants->pdata, variants->len);
  g_assert_false (emer_persistent_cache_has_more (cache, token));

  assert_variants_removed (cache, (GVariant **) variants->pdata, variants->len);
  assert_cache_is_empty (cache);

  g_ptr_array_unref (variants);
  g_object_unref (cache);
}

static void
test_persistent_cache_remove_none (gboolean     *unused,
                                   gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  assert_variants_stored (cache, NULL /* variants */, 0);
  assert_variants_removed (cache, NULL /* variants */, 0);
  assert_cache_is_empty (cache);

  g_object_unref (cache);
}

static void
test_persistent_cache_remove_one (gboolean     *unused,
                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  GVariant *variant = make_variant (0);
  g_variant_ref_sink (variant);
  GVariant *variants[] = { variant };
  gsize num_variants = G_N_ELEMENTS (variants);
  assert_variants_stored (cache, variants, num_variants);
  assert_variants_removed (cache, variants, num_variants);
  g_variant_unref (variant);

  assert_cache_is_empty (cache);
  g_object_unref (cache);
}

static void
test_persistent_cache_remove_many (gboolean     *unused,
                                   gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  GPtrArray *variants = make_many_variants ();

  assert_variants_stored (cache, (GVariant **) variants->pdata, variants->len);
  assert_variants_removed (cache, (GVariant **) variants->pdata, variants->len);
  g_ptr_array_unref (variants);

  assert_cache_is_empty (cache);
  g_object_unref (cache);
}

static void
test_persistent_cache_remove_when_empty (gboolean     *unused,
                                         gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  assert_variants_removed (cache, NULL /* variants */, 0);
  assert_cache_is_empty (cache);

  g_object_unref (cache);
}

static void
test_persistent_cache_purges_when_out_of_date (gboolean     *unused,
                                               gconstpointer dontuseme)
{
  EmerCacheSizeProvider *cache_size_provider =
    emer_cache_size_provider_new_full (TEST_DIRECTORY TEST_CACHE_SIZE_FILE);
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new (NULL);
  GError *error = NULL;
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_individual.metrics. Error: *.");
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_aggregate.metrics. Error: *.");
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Failed to unlink old cache file " TEST_DIRECTORY
                         "cache_sequence.metrics. Error: *.");
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (TEST_DIRECTORY, cache_size_provider,
                                    boot_id_provider, cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL, &error);
  g_test_assert_expected_messages ();
  g_assert_no_error (error);
  g_assert_nonnull (cache);

  g_object_unref (cache_size_provider);
  g_object_unref (boot_id_provider);

  GPtrArray *variants = make_many_variants ();
  assert_variants_stored (cache, (GVariant **) variants->pdata, variants->len);
  g_ptr_array_unref (variants);

  gint current_version;
  gboolean get_succeeded =
    emer_cache_version_provider_get_version (cache_version_provider,
                                             &current_version);
  g_assert_true (get_succeeded);

  gboolean set_succeeded =
    emer_cache_version_provider_set_version (cache_version_provider,
                                             current_version - 1, &error);
  g_assert_no_error (error);
  g_assert_true (set_succeeded);

  g_object_unref (cache_version_provider);
  g_object_unref (cache);

  EmerPersistentCache *cache2 = make_testing_cache ();
  assert_cache_is_empty (cache2);
  g_object_unref (cache2);
}

/*
 * Ensures that the persistent cache creates a new metadata file should one not
 * be found.
 *
 * Performs no special mutation of the metadata file beyond what the production
 * code would normally do. Thus, the offset should always be reset to, and then
 * cached in memory to, 0.
 */
static void
test_persistent_cache_builds_boot_metadata_file (gboolean     *unused,
                                                 gconstpointer dontuseme)
{
  gint64 absolute_time, relative_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time));
  g_assert_true (emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time));

  EmerPersistentCache *cache = make_testing_cache ();

  gint64 offset = read_offset ();

  g_assert_true (boot_timestamp_is_valid (relative_time, absolute_time));

  // The offset should not have changed.
  g_assert_cmpint (offset, ==, 0);
  g_assert_true (boot_offset_was_reset ());

  g_object_unref (cache);
}

/*
 * Triggers the computation of a new boot offset by initializing a cache with no
 * preexisting boot metadata file, which resets the offset to 0. Then unrefs the
 * persistent cache and makes it anew, clearing its in-memory cache. Then
 * mutates the metadata file to simulate a new boot without modifying the
 * timestamps, prompting a newly instantiated persistent cache to compute a new
 * boot offset that should be approximately 0.
 */
static void
test_persistent_cache_computes_reasonable_offset (gboolean     *unused,
                                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  gint64 first_offset = read_offset ();
  g_assert_true (boot_offset_was_reset ());

  gint64 absolute_time, relative_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time));
  g_assert_true (emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time));

  g_object_unref (cache);

  // Mutate boot id directly because we cannot actually reboot in a test case.
  set_boot_id_in_metadata_file (FAKE_BOOT_ID);

  EmerPersistentCache *cache2 = make_testing_cache ();

  g_assert_true (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset ();
  gint64 max_second_offset = first_offset + ACCEPTABLE_OFFSET_VARIANCE;
  gint64 min_second_offset = first_offset - ACCEPTABLE_OFFSET_VARIANCE;
  g_assert_cmpint (second_offset, <=, max_second_offset);
  g_assert_cmpint (second_offset, >=, min_second_offset);

  // This should not have simply reset the metadata file again.
  g_assert_false (boot_offset_was_reset ());

  g_object_unref (cache2);
}

/*
 * Tests that releasing and recreating the persistent cache within the same boot
 * doesn't change the boot offset.
 *
 * Triggers the computation of a new boot offset by asking for the new boot
 * offset with no preexisting boot metadata file, which triggers a reset to
 * offset 0 and the current boot id. Then unrefs the persistent cache and makes
 * it anew, clearing its in-memory cache. Then mutates the metadata file to
 * simulate a new boot. Then requests the boot offset again to prompt the
 * persistent cache to compute a new offset. Then unrefs and recreates the
 * persistent cache again to clear its in-memory cache. Finally, requests the
 * boot offset, prompting the persistent cache to write new timestamps without
 * recomputing the boot offset.
 */
static void
test_persistent_cache_does_not_compute_offset_when_boot_id_is_same (gboolean     *unused,
                                                                    gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  g_assert_true (boot_offset_was_reset ());

  g_object_unref (cache);
  set_boot_id_in_metadata_file (FAKE_BOOT_ID);

  gint64 relative_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time));
  gint64 absolute_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time));

  EmerPersistentCache *cache2 = make_testing_cache ();

  g_assert_true (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset ();

  // This should not have simply reset the metadata file again.
  g_assert_false (boot_offset_was_reset ());

  g_object_unref (cache2);
  EmerPersistentCache *cache3 = make_testing_cache ();

  gint64 third_offset = read_offset ();
  g_assert_cmpint (third_offset, ==, second_offset);

  g_object_unref (cache3);
}

/*
 * Tests that the in-memory cache is used when present by changing the boot
 * offset on disk between calls to the same instance of a persistent cache.
 */
static void
test_persistent_cache_reads_cached_boot_offset (gboolean     *unused,
                                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  gint64 first_offset;
  GError *error = NULL;
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, &first_offset, &error);
  g_assert_no_error (error);
  g_assert_true (get_succeeded);

  /*
   * This value should never be read because the persistent cache should read
   * from its in-memory cache instead.
   */
  set_boot_offset_in_metadata_file (FAKE_BOOT_OFFSET);

  gint64 second_offset;

  /*
   * This call should read the offset from its in-memory cache, not the new one
   * on disk.
   */
  get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, &second_offset, &error);
  g_assert_no_error (error);
  g_assert_true (get_succeeded);

  g_assert_cmpint (first_offset, ==, second_offset);

  g_object_unref (cache);
}

/*
 * Ensures that releasing the persistent cache prompts it to update the
 * timestamps in the metadata file.
 */
static void
test_persistent_cache_updates_timestamps_on_finalize (gboolean     *unused,
                                                      gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  gint64 relative_time = read_relative_time ();
  gint64 absolute_time = read_absolute_time ();

  // Make a little time pass.
  g_usleep (75000); // 0.075 seconds

  // This call should update the timestamps in the metadata file.
  g_object_unref (cache);

  // These timestamps should have increased.
  g_assert_cmpint (relative_time, <, read_relative_time ());
  g_assert_cmpint (absolute_time, <, read_absolute_time ());
}

/*
 * Ensures that the get_boot_time_offset call will reset the metadata file if
 * one isn't found.
 */
static void
test_persistent_cache_get_offset_can_build_boot_metadata_file (gboolean     *unused,
                                                               gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  /*
   * Don't write a default boot offset file. We want to create a new one via
   * production code.
   */

  GError *error = NULL;
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error);

  g_assert_no_error (error);
  g_assert_true (get_succeeded);

  // The previous request should have reset the metadata file.
  g_assert_true (boot_offset_was_reset ());

  g_object_unref (cache);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);

// We are using a gboolean as a fixture type, but it will go unused.
#define ADD_CACHE_TEST_FUNC(path, func) \
  g_test_add((path), gboolean, NULL, setup, (func), teardown)

  ADD_CACHE_TEST_FUNC ("/persistent-cache/new", test_persistent_cache_new);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/cost", test_persistent_cache_cost);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-none",
                       test_persistent_cache_store_none);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-one",
                       test_persistent_cache_store_one);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-many",
                       test_persistent_cache_store_many);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-when-full",
                       test_persistent_cache_store_when_full);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/read-none",
                       test_persistent_cache_read_none);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/read-one",
                       test_persistent_cache_read_one);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/read-many",
                       test_persistent_cache_read_many);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/read-when-empty",
                       test_persistent_cache_read_when_empty);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/has-more",
                       test_persistent_cache_has_more);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/remove-none",
                       test_persistent_cache_remove_none);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/remove-one",
                       test_persistent_cache_remove_one);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/remove-many",
                       test_persistent_cache_remove_many);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/remove-when-empty",
                       test_persistent_cache_remove_when_empty);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/purges-when-out-of-date",
                       test_persistent_cache_purges_when_out_of_date);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/builds-boot-metadata-file",
                       test_persistent_cache_builds_boot_metadata_file);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/computes-reasonable-offset",
                       test_persistent_cache_computes_reasonable_offset);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/does-not-compute-offset-when-boot-id-is-same",
                       test_persistent_cache_does_not_compute_offset_when_boot_id_is_same);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/reads-cached-boot-offset",
                       test_persistent_cache_reads_cached_boot_offset);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/updates-timestamps-on-finalize",
                       test_persistent_cache_updates_timestamps_on_finalize);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/get-offset-can-build-boot-metadata-file",
                       test_persistent_cache_get_offset_can_build_boot_metadata_file);
#undef ADD_CACHE_TEST_FUNC

  return g_test_run ();
}
