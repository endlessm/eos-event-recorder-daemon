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

#include "daemon/emer-persistent-cache.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <eosmetrics/eosmetrics.h>

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

// TODO: Replace this with a reasonable value once it is used.
#define MAX_BYTES_TO_READ 0

#define ACCEPTABLE_OFFSET_VARIANCE 500000000 // 500 milliseconds

#define DEFAULT_BOOT_OFFSET_KEY_FILE_DATA \
  "[time]\n" \
  "boot_offset=0\n" \
  "was_reset=true\n" \
  "absolute_time=1403195800943262692\n" \
  "relative_time=2516952859775\n" \
  "boot_id=299a89b4-72c2-455a-b2d3-13c1a7c8c11f\n"

#define CACHE_SIZE_KEY_FILE_DATA \
  "[persistent_cache_size]\n" \
  "maximum=92160\n"

#define DEFAULT_CACHE_VERSION_KEY_FILE_DATA \
  "[cache_version_info]\n" \
  "version=2\n"

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
write_empty_metrics_file (gchar *suffix)
{
  gchar *path = g_strconcat (TEST_DIRECTORY, CACHE_PREFIX, suffix, NULL);
  GFile *file = g_file_new_for_path (path);
  g_free (path);

  GError *error = NULL;
  g_file_replace_contents (file, "", 0, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL,
                           &error);
  g_assert_no_error (error);
  g_object_unref (file);
}

static void
write_empty_metrics_files (void)
{
  write_empty_metrics_file (INDIVIDUAL_SUFFIX);
  write_empty_metrics_file (AGGREGATE_SUFFIX);
  write_empty_metrics_file (SEQUENCE_SUFFIX);
}

static void
teardown (gboolean     *unused,
          gconstpointer dontuseme)
{
  g_unlink (TEST_DIRECTORY CACHE_PREFIX INDIVIDUAL_SUFFIX);
  g_unlink (TEST_DIRECTORY CACHE_PREFIX AGGREGATE_SUFFIX);
  g_unlink (TEST_DIRECTORY CACHE_PREFIX SEQUENCE_SUFFIX);
  g_unlink (TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE);
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
  write_empty_metrics_files ();
}

static EmerPersistentCache *
make_testing_cache (void)
{
  GError *error = NULL;
  EmerCacheSizeProvider *cache_size_provider =
    emer_cache_size_provider_new_full (TEST_DIRECTORY TEST_CACHE_SIZE_FILE);
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new_full (TEST_DIRECTORY TEST_CACHE_VERSION_FILE);
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (NULL, &error, TEST_DIRECTORY,
                                    cache_size_provider, boot_id_provider,
                                    cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL);
  g_assert_no_error (error);

  g_object_unref (cache_size_provider);
  g_object_unref (boot_id_provider);
  g_object_unref (cache_version_provider);

  return cache;
}

/*
 * Returns a new GKeyFile associated with the boot timing metadata file.
 * Keyfile should be unref'd via g_key_file_unref().
 */
static GKeyFile *
load_testing_key_file (void)
{
  GKeyFile *key_file = g_key_file_new ();
  gchar *full_path =
    g_strconcat (TEST_DIRECTORY, BOOT_OFFSET_METADATA_FILE, NULL);
  gboolean load_succeeded =
    g_key_file_load_from_file (key_file, full_path, G_KEY_FILE_NONE, NULL);
  g_assert_true (load_succeeded);
  return key_file;
}

/*
 * Will overwrite the contents of the boot id metadata file's boot offset with a
 * given new_offset.
 */
static void
set_boot_offset_in_metadata_file (gint64 new_offset)
{
  GKeyFile *key_file = load_testing_key_file ();

  g_key_file_set_int64 (key_file,
                        CACHE_TIMING_GROUP_NAME,
                        CACHE_BOOT_OFFSET_KEY,
                        new_offset);

  gboolean save_succeeded =
    g_key_file_save_to_file (key_file, TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE,
                             NULL);
  g_assert_true (save_succeeded);
  g_key_file_unref (key_file);
}

/*
 * Populates the boot metadata file with data similar to the defaults that will
 * be written when it's reset.
 * Must be called AFTER the testing directory exists (after a persistent cache
 * instance has been constructed).
 */
static void
write_default_boot_offset_key_file_to_disk (void)
{
  GKeyFile *key_file = g_key_file_new ();
  gboolean load_succeeded =
    g_key_file_load_from_data (key_file, DEFAULT_BOOT_OFFSET_KEY_FILE_DATA, -1,
                               G_KEY_FILE_NONE, NULL);
  g_assert_true (load_succeeded);

  gboolean save_succeeded =
    g_key_file_save_to_file (key_file, TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE,
                             NULL);
  g_assert_true (save_succeeded);
}

/*
 * Overwrites the metadata file's boot id with the given boot id.
 */
static void
set_boot_id_in_metadata_file (gchar *boot_id)
{
  GKeyFile *key_file = load_testing_key_file ();

  g_key_file_set_string (key_file,
                         CACHE_TIMING_GROUP_NAME,
                         CACHE_LAST_BOOT_ID_KEY,
                         boot_id);

  gboolean save_succeeded =
    g_key_file_save_to_file (key_file, TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE,
                             NULL);
  g_assert_true (save_succeeded);
  g_key_file_unref (key_file);
}

/*
 * Removes the offset key/value pair from the boot metadata file to simulate
 * corruption and writes that change to disk.
 */
static void
remove_offset (void)
{
  GKeyFile *key_file = load_testing_key_file ();
  gboolean remove_succeeded =
    g_key_file_remove_key (key_file, CACHE_TIMING_GROUP_NAME,
                           CACHE_BOOT_OFFSET_KEY, NULL);
  g_assert_true (remove_succeeded);

  gboolean save_succeeded =
    g_key_file_save_to_file (key_file, TEST_DIRECTORY BOOT_OFFSET_METADATA_FILE,
                             NULL);
  g_assert_true (save_succeeded);
  g_key_file_unref (key_file);
}

/*
 * Gets the stored offset from the metadata file.
 */
static gint64
read_offset (void)
{
  GKeyFile *key_file = load_testing_key_file ();
  GError *error = NULL;
  gint64 stored_offset = g_key_file_get_int64 (key_file,
                                               CACHE_TIMING_GROUP_NAME,
                                               CACHE_BOOT_OFFSET_KEY,
                                               &error);
  g_key_file_unref (key_file);
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
  GKeyFile *key_file = load_testing_key_file ();
  GError *error = NULL;
  gboolean was_reset = g_key_file_get_boolean (key_file,
                                               CACHE_TIMING_GROUP_NAME,
                                               CACHE_WAS_RESET_KEY,
                                               &error);
  g_key_file_unref (key_file);
  g_assert_no_error (error);
  return was_reset && (read_offset() == 0);
}

/*
 * Gets the stored relative time from the metadata file.
 */
static gint64
read_relative_time (void)
{
  GKeyFile *key_file = load_testing_key_file ();
  GError *error = NULL;
  gint64 stored_offset = g_key_file_get_int64 (key_file,
                                               CACHE_TIMING_GROUP_NAME,
                                               CACHE_RELATIVE_TIME_KEY,
                                               &error);
  g_key_file_unref (key_file);
  g_assert_no_error (error);
  return stored_offset;
}

/*
 * Gets the stored absolute time from the metadata file.
 */
static gint64
read_absolute_time (void)
{
  GKeyFile *key_file = load_testing_key_file ();
  GError *error = NULL;
  gint64 stored_offset = g_key_file_get_int64 (key_file,
                                               CACHE_TIMING_GROUP_NAME,
                                               CACHE_ABSOLUTE_TIME_KEY,
                                               &error);
  g_key_file_unref (key_file);
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

static void
event_value_own (EventValue *event_value)
{
  GVariant *auxiliary_payload = event_value->auxiliary_payload;
  if (auxiliary_payload != NULL)
    g_variant_ref_sink (auxiliary_payload);
}

static void
singular_event_own (SingularEvent *singular)
{
  EventValue *event_value = &singular->event_value;
  event_value_own (event_value);
}

static void
aggregate_event_own (AggregateEvent *aggregate)
{
  SingularEvent *event = &aggregate->event;
  singular_event_own (event);
}

static void
sequence_event_own (SequenceEvent *sequence)
{
  for (gint i = 0; i < sequence->num_event_values; i++)
    event_value_own (sequence->event_values + i);
}

static void
singular_buffer_own (SingularEvent *singular_buffer,
                     gint           num_singulars_buffered)
{
  for (gint i = 0; i < num_singulars_buffered; i++)
    singular_event_own (singular_buffer + i);
}

static void
aggregate_buffer_own (AggregateEvent *aggregate_buffer,
                      gint            num_aggregates_buffered)
{
  for (gint i = 0; i < num_aggregates_buffered; i++)
    aggregate_event_own (aggregate_buffer + i);
}

static void
sequence_buffer_own (SequenceEvent *sequence_buffer,
                     gint           num_sequences_buffered)
{
  for (gint i = 0; i < num_sequences_buffered; i++)
    sequence_event_own (sequence_buffer + i);
}

static void
make_event_value (gint        choice,
                  EventValue *event_value)
{
  switch (choice)
    {
    case 0:
      {
        EventValue value =
          { G_GINT64_CONSTANT (1876), g_variant_new_double (3.14159) };
        *event_value = value;
        break;
      }

    case 1:
      {
        EventValue value =
          {
            G_GINT64_CONSTANT (0),
            g_variant_new_string ("negative-1-point-steve")
          };
        *event_value = value;
        break;
      }

    case 2:
      {
        EventValue value = { G_GINT64_CONSTANT (-1), NULL };
        *event_value = value;
        break;
      }

    case 3:
      {
        EventValue value =
          { G_GINT64_CONSTANT (7), g_variant_new_double (2.71828) };
        *event_value = value;
        break;
      }

    case 4:
      {
        EventValue value =
          {
            G_GINT64_CONSTANT (67352),
            g_variant_new_string ("Help! I'm trapped in a testing string!")
          };
        *event_value = value;
        break;
      }

    case 5:
      {
        EventValue value = { G_GINT64_CONSTANT (747), NULL };
        *event_value = value;
        break;
      }

    case 6:
      {
        EventValue value =
          {
            G_GINT64_CONSTANT (57721),
            g_variant_new_string ("Secret message to the Russians: The "
                                  "'rooster' has 'laid' an 'egg'.")
          };
        *event_value = value;
        break;
      }

    case 7:
      {
        EventValue value =
          { G_GINT64_CONSTANT (-100), g_variant_new_double (120.20569) };
        *event_value = value;
        break;
      }

    case 8:
      {
        EventValue value =
          { G_GINT64_CONSTANT (127384), g_variant_new_double (-2.685452) };
        *event_value = value;
        break;
      }

    default:
      {
        g_error ("Tried to use a choice for make_event_values that hasn't been "
                 "programmed.");
      }
    }
}

static void
make_singular_event (gint           choice,
                     SingularEvent *singular)
{
  switch (choice)
    {
    case 0:
      {
        EventValue event_value =
          { G_GINT64_CONSTANT (42), g_variant_new_string ("murphy") };
        SingularEvent singular_event =
          {
            234u,
            { 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe,
              0xef, 0xde, 0xad, 0xbe, 0xef },
            event_value
          };
        *singular = singular_event;
        break;
      }

    case 1:
      {
        EventValue event_value =
          { G_GINT64_CONSTANT (999), g_variant_new_int32 (404) };
        SingularEvent singular_event =
          {
            121u,
            { 0x01, 0x23, 0x45, 0x67, 0x89, 0x01, 0x23, 0x45, 0x67, 0x89, 0x01,
              0x23, 0x45, 0x67, 0x89, 0x01 },
            event_value
          };
        *singular = singular_event;
        break;
      }

    case 2:
      {
        EventValue event_value =
          {
            G_GINT64_CONSTANT (12012),
            g_variant_new_string ("I am a banana!")
          };
        SingularEvent singular_event =
          {
            555u,
            { 0x4b, 0x4b, 0x4b, 0x4b, 0x4b, 0x4b, 0x4b, 0x4b, 0x4b, 0x4b, 0x4b,
              0x4b, 0x4b, 0x4b, 0x4b, 0x4b },
            event_value
          };
        *singular = singular_event;
        break;
      }

    case 3:
      {
        EventValue event_value =
          { G_GINT64_CONSTANT (-128), g_variant_new_int32 (64) };
        SingularEvent singular_event =
          {
            411u,
            { 0x55, 0x2c, 0x55, 0x2c, 0x55, 0x2c, 0x55, 0x2c, 0x55, 0x2c, 0x55,
              0x2c, 0x55, 0x2c, 0x55, 0x2c },
            event_value
          };
        *singular = singular_event;
        break;
      }

    default:
      {
        g_error ("Tried to use a choice for make_singular_event that hasn't "
                 "been programmed.");
      }
    }
}

static void
make_aggregate_event (gint            choice,
                      AggregateEvent *aggregate)
{
  switch (choice)
    {
    case 0:
      {
        EventValue event_value =
          { G_GINT64_CONSTANT (9876), g_variant_new_string ("meepo") };
        SingularEvent event =
          {
            12u,
            { 0xde, 0xaf, 0x00, 0x01, 0xde, 0xaf, 0x00, 0x01, 0xde, 0xaf, 0x00,
              0x01, 0xde, 0xaf, 0x00, 0x01 },
            event_value
          };
        AggregateEvent aggregate_event = { event, G_GINT64_CONSTANT (111) };
        *aggregate = aggregate_event;
        break;
      }

    case 1:
      {
        EventValue event_value =
          {
            G_GINT64_CONSTANT (-333),
            g_variant_new_string ("My spoon is too big.")
          };
        SingularEvent event =
          {
            1019u,
            { 0x33, 0x44, 0x95, 0x2a, 0x33, 0x44, 0x95, 0x2a, 0x33, 0x44, 0x95,
              0x2a, 0x33, 0x44, 0x95, 0x2a },
            event_value
          };
        AggregateEvent aggregate_event = { event, G_GINT64_CONSTANT (1) };
        *aggregate = aggregate_event;
        break;
      }

    case 2:
      {
        EventValue event_value =
          { G_GINT64_CONSTANT (5965), g_variant_new_string ("!^@#@#^#$") };
        SingularEvent event =
          {
            5u,
            { 0x33, 0x44, 0x95, 0x2a, 0xb4, 0x9c, 0x2d, 0x14, 0x45, 0xaa, 0x33,
              0x44, 0x95, 0x2a, 0xb4, 0x9c },
            event_value
          };
        AggregateEvent aggregate_event = { event, G_GINT64_CONSTANT (-3984) };
        *aggregate = aggregate_event;
        break;
      }

    default:
      {
        g_error ("Tried to use a choice for make_aggregate_event that hasn't "
                 "been programmed.");
      }
    }
}

static void
make_sequence_event (gint           choice,
                     SequenceEvent *sequence)
{
  switch (choice)
    {
    case 0:
      {
        EventValue *event_values = g_new (EventValue, 3);
        for (gint i = 0; i < 3; i++)
          make_event_value (i, event_values + i);

        SequenceEvent sequence_event =
          {
            1277u,
            { 0x13, 0x37, 0x13, 0x37, 0x13, 0x37, 0x13, 0x37, 0x13, 0x37, 0x13,
              0x37, 0x13, 0x37, 0x13, 0x37 },
            event_values,
            3
          };
        *sequence = sequence_event;
        break;
      }

    case 1:
      {
        EventValue *event_values = g_new (EventValue, 2);
        for (gint i = 0; i < 2; i++)
          make_event_value (i + 3, event_values + i);

        SequenceEvent sequence_event =
          {
            91912u,
            { 0x13, 0x37, 0xd0, 0x0d, 0x13, 0x37, 0xd0, 0x0d, 0x13, 0x37, 0xd0,
              0x0d, 0x13, 0x37, 0xd0, 0x0d },
            event_values,
            2
          };
        *sequence = sequence_event;
        break;
      }

    case 2:
      {
        EventValue *event_values = g_new (EventValue, 4);
        for (gint i = 0; i < 4; i++)
          make_event_value (i + 5, event_values + i);

        SequenceEvent sequence_event =
          {
            113u,
            { 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1,
              0xe1, 0xe1, 0xe1, 0xe1, 0xe1 },
            event_values,
            4
          };
        *sequence = sequence_event;
        break;
      }

    default:
      {
        g_error ("Tried to use a choice for make_sequence_event that hasn't "
                 "been programmed.");
      }
    }
}

/*
 * Stores a single singular event and asserts that it (and nothing else) was
 * stored.
 */
static gboolean
store_single_singular_event (EmerPersistentCache *cache,
                             capacity_t          *capacity)
{
  SingularEvent singular_array[1];
  make_singular_event (0, singular_array);

  AggregateEvent aggregate_array[] = {};
  SequenceEvent sequence_array [] = {};

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;

  gboolean success =
    emer_persistent_cache_store_metrics (cache,
                                         singular_array,
                                         aggregate_array,
                                         sequence_array,
                                         1, 0, 0,
                                         &num_singulars_stored,
                                         &num_aggregates_stored,
                                         &num_sequences_stored,
                                         capacity);

  g_assert_cmpint (num_singulars_stored, ==, 1);
  g_assert_cmpint (num_aggregates_stored, ==, 0);
  g_assert_cmpint (num_sequences_stored, ==, 0);

  return success;
}

/*
 * Stores a single aggregate event and asserts that it (and nothing else) was
 * stored.
 */
static gboolean
store_single_aggregate_event (EmerPersistentCache *cache,
                              capacity_t          *capacity)
{
  SingularEvent singular_array[] = {};

  AggregateEvent aggregate_array[1];
  make_aggregate_event (0, aggregate_array);

  SequenceEvent sequence_array[] = {};

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;

  gboolean success =
    emer_persistent_cache_store_metrics (cache,
                                         singular_array,
                                         aggregate_array,
                                         sequence_array,
                                         0, 1, 0,
                                         &num_singulars_stored,
                                         &num_aggregates_stored,
                                         &num_sequences_stored,
                                         capacity);

  g_assert_cmpint (num_singulars_stored, ==, 0);
  g_assert_cmpint (num_aggregates_stored, ==, 1);
  g_assert_cmpint (num_sequences_stored, ==, 0);

  return success;
}

/*
 * Stores a single sequence event and asserts that it (and nothing else) was
 * stored.
 */
static gboolean
store_single_sequence_event (EmerPersistentCache *cache,
                             capacity_t          *capacity)
{
  SingularEvent singular_array[] = {};
  AggregateEvent aggregate_array[] = {};

  SequenceEvent sequence_array[1];
  make_sequence_event (0, sequence_array);

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;

  gboolean success =
    emer_persistent_cache_store_metrics (cache,
                                         singular_array,
                                         aggregate_array,
                                         sequence_array,
                                         0, 0, 1,
                                         &num_singulars_stored,
                                         &num_aggregates_stored,
                                         &num_sequences_stored,
                                         capacity);

  g_assert_cmpint (num_singulars_stored, ==, 0);
  g_assert_cmpint (num_aggregates_stored, ==, 0);
  g_assert_cmpint (num_sequences_stored, ==, 1);

  return success;
}

static void
make_many_events (SingularEvent  **singular_array,
                  AggregateEvent **aggregate_array,
                  SequenceEvent  **sequence_array,
                  gint            *num_singulars_made,
                  gint            *num_aggregates_made,
                  gint            *num_sequences_made)
{
  *num_singulars_made = 4;
  *singular_array = g_new (SingularEvent, *num_singulars_made);
  for (gint i = 0; i < *num_singulars_made; i++)
    make_singular_event (i, *singular_array + i);

  *num_aggregates_made = 3;
  *aggregate_array = g_new (AggregateEvent, *num_aggregates_made);
  for (gint i = 0; i < *num_aggregates_made; i++)
    make_aggregate_event (i, *aggregate_array + i);

  *num_sequences_made = 3;
  *sequence_array = g_new (SequenceEvent, *num_sequences_made);
  for (gint i = 0; i < *num_sequences_made; i++)
    make_sequence_event (i, *sequence_array + i);
}

static void
free_variant_c_array (GVariant *array[])
{
  g_return_if_fail (array != NULL);

  for (gint i = 0; array[i] != NULL; i++)
    g_variant_unref (array[i]);
  g_free (array);
}

static gint
c_array_len (GVariant *array[])
{
  gint i;
  for (i = 0; array[i] != NULL; i++)
    ; // Do nothing.
  return i;
}

static gboolean
store_many (EmerPersistentCache *cache,
            gint                *num_singulars_made,
            gint                *num_aggregates_made,
            gint                *num_sequences_made,
            gint                *num_singulars_stored,
            gint                *num_aggregates_stored,
            gint                *num_sequences_stored,
            capacity_t          *capacity)
{
  SingularEvent *singular_array;
  AggregateEvent *aggregate_array;
  SequenceEvent *sequence_array;

  make_many_events (&singular_array,
                    &aggregate_array,
                    &sequence_array,
                    num_singulars_made,
                    num_aggregates_made,
                    num_sequences_made);

  gboolean success = emer_persistent_cache_store_metrics (cache,
                                                          singular_array,
                                                          aggregate_array,
                                                          sequence_array,
                                                          *num_singulars_made,
                                                          *num_aggregates_made,
                                                          *num_sequences_made,
                                                          num_singulars_stored,
                                                          num_aggregates_stored,
                                                          num_sequences_stored,
                                                          capacity);

  g_free (singular_array);
  g_free (aggregate_array);
  for (gint i = 0; i < *num_sequences_made; i++)
    g_free (sequence_array[i].event_values);
  g_free (sequence_array);

  return success;
}

static void
assert_singulars_equal_variants (SingularEvent *singular_array,
                                 gint           singular_array_length,
                                 GVariant     **variants)
{
  for (gint i = 0; i < singular_array_length || variants[i] != NULL; i++)
    {
      g_assert_true (i < singular_array_length && variants[i] != NULL);

      GVariant *singular_variant = singular_to_variant (singular_array + i);
      g_assert_true (g_variant_equal (singular_variant, variants[i]));
      g_variant_unref (singular_variant);
    }
}

static void
assert_aggregates_equal_variants (AggregateEvent *aggregate_array,
                                  gint            aggregate_array_length,
                                  GVariant      **variants)
{
  for (gint i = 0; i < aggregate_array_length || variants[i] != NULL; i++)
    {
      g_assert_true (i < aggregate_array_length && variants[i] != NULL);

      GVariant *aggregate_variant = aggregate_to_variant (aggregate_array + i);
      g_assert_true (g_variant_equal (aggregate_variant, variants[i]));
      g_variant_unref (aggregate_variant);
    }
}

static void
assert_sequences_equal_variants (SequenceEvent *sequence_array,
                                 gint           sequence_array_length,
                                 GVariant     **variants)
{
  for (gint i = 0; i < sequence_array_length || variants[i] != NULL; i++)
    {
      g_assert_true (i < sequence_array_length && variants[i] != NULL);

      GVariant *sequence_variant = sequence_to_variant (sequence_array + i);
      g_assert_true (g_variant_equal (sequence_variant, variants[i]));
      g_variant_unref (sequence_variant);
    }
}

// ----- Actual Test Cases below ------

static void
test_persistent_cache_new_succeeds (gboolean     *unused,
                                    gconstpointer dontuseme)
{
  GError *error = NULL;
  EmerPersistentCache *cache = make_testing_cache ();
  g_assert_nonnull (cache);
  g_object_unref (cache);
  if (error != NULL)
    g_error_free (error);
  g_assert_no_error (error);
}

/*
 * Test ensures the store function properly sets its out parameters, even if
 * no metrics are being stored via the call.
 */
static void
test_persistent_cache_store_sets_out_parameters (gboolean     *unused,
                                                 gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  capacity_t capacity = 700; // Totally invalid value!
  SingularEvent singular_array[] = {};
  AggregateEvent aggregate_array[] = {};
  SequenceEvent sequence_array [] = {};

  // Arbitrary values that should be overwritten:
  gint num_singulars_stored = -1;
  gint num_aggregates_stored = -50;
  gint num_sequences_stored = 555;

  g_assert_true (emer_persistent_cache_store_metrics (cache,
                                                      singular_array,
                                                      aggregate_array,
                                                      sequence_array,
                                                      0, 0, 0,
                                                      &num_singulars_stored,
                                                      &num_aggregates_stored,
                                                      &num_sequences_stored,
                                                      &capacity));

  // An empty cache should be in the LOW capacity state.
  g_assert_cmpint (capacity, ==, CAPACITY_LOW);
  g_assert_cmpint (num_singulars_stored, ==, 0);
  g_assert_cmpint (num_aggregates_stored, ==, 0);
  g_assert_cmpint (num_sequences_stored, ==, 0);

  g_object_unref (cache);
}

static void
test_persistent_cache_store_one_singular_event_succeeds (gboolean     *unused,
                                                         gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  g_assert_true (store_single_singular_event (cache, &capacity));
  g_object_unref (cache);
  g_assert_cmpint (capacity, ==, CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_aggregate_event_succeeds (gboolean     *unused,
                                                          gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  g_assert_true (store_single_aggregate_event (cache, &capacity));
  g_object_unref (cache);
  g_assert_cmpint (capacity, ==, CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_sequence_event_succeeds (gboolean     *unused,
                                                         gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  g_assert_true (store_single_sequence_event (cache, &capacity));
  g_object_unref (cache);
  g_assert_cmpint (capacity, ==, CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_of_each_succeeds (gboolean     *unused,
                                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  SingularEvent singular_array[1];
  make_singular_event (0, singular_array);

  AggregateEvent aggregate_array[1];
  make_aggregate_event (0, aggregate_array);

  SequenceEvent sequence_array[1];
  make_sequence_event (0, sequence_array);

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;

  gboolean success =
    emer_persistent_cache_store_metrics (cache,
                                         singular_array,
                                         aggregate_array,
                                         sequence_array,
                                         1, 1, 1,
                                         &num_singulars_stored,
                                         &num_aggregates_stored,
                                         &num_sequences_stored,
                                         &capacity);
  g_object_unref (cache);

  g_free (sequence_array[0].event_values);

  g_assert_true (success);

  g_assert_cmpint (num_singulars_stored, ==, 1);
  g_assert_cmpint (num_aggregates_stored, ==, 1);
  g_assert_cmpint (num_sequences_stored, ==, 1);

  g_assert_cmpint (capacity, ==, CAPACITY_LOW);
}

static void
test_persistent_cache_store_many_succeeds (gboolean     *unused,
                                           gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  gint num_singulars_made, num_aggregates_made, num_sequences_made;
  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;
  gboolean success =
    store_many (cache, &num_singulars_made, &num_aggregates_made,
                &num_sequences_made, &num_singulars_stored,
                &num_aggregates_stored, &num_sequences_stored, &capacity);
  g_object_unref (cache);
  g_assert_true (success);

  g_assert_cmpint (num_singulars_stored, ==, num_singulars_made);
  g_assert_cmpint (num_aggregates_stored, ==, num_aggregates_made);
  g_assert_cmpint (num_sequences_stored, ==, num_sequences_made);
}

static void
test_persistent_cache_store_when_full_succeeds (gboolean     *unused,
                                                gconstpointer dontuseme)
{
  GError *error = NULL;
  EmerCacheSizeProvider *cache_size_provider =
    emer_cache_size_provider_new_full (TEST_DIRECTORY TEST_CACHE_SIZE_FILE);
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new_full (TEST_DIRECTORY TEST_CACHE_VERSION_FILE);
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (NULL, &error, TEST_DIRECTORY,
                                    cache_size_provider, boot_id_provider,
                                    cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL);
  g_assert_no_error (error);

  g_object_unref (cache_version_provider);
  g_object_unref (boot_id_provider);

  capacity_t capacity = CAPACITY_LOW;

  // Store a ton, until it is full.
  // TODO: Find a less hacky way of doing this.
  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (cache_size_provider);
  g_object_unref (cache_size_provider);
  gint iterations = max_cache_size / 150;
  for (gint i = 0; i < iterations; i++)
    {
      gint num_singulars_made, num_aggregates_made, num_sequences_made;
      gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
      gboolean success =
        store_many (cache, &num_singulars_made, &num_aggregates_made,
                    &num_sequences_made, &num_singulars_stored,
                    &num_aggregates_stored, &num_sequences_stored, &capacity);
      g_assert_true (success);

      if (capacity == CAPACITY_MAX)
        {
          g_assert_cmpint (num_singulars_stored, <=, num_singulars_made);
          g_assert_cmpint (num_aggregates_stored, <=, num_aggregates_made);
          g_assert_cmpint (num_sequences_stored, <=, num_sequences_made);
          break;
        }

      g_assert_cmpint (num_singulars_stored, ==, num_singulars_made);
      g_assert_cmpint (num_aggregates_stored, ==, num_aggregates_made);
      g_assert_cmpint (num_sequences_stored, ==, num_sequences_made);
    }

  g_object_unref (cache);
  g_assert_cmpint (capacity, ==, CAPACITY_MAX);
}

static void
test_persistent_cache_drain_one_singular_event_succeeds (gboolean     *unused,
                                                         gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  SingularEvent singulars_stored[1];
  make_singular_event (1, singulars_stored);
  singular_event_own (singulars_stored);

  AggregateEvent aggregates_stored[] = {};
  SequenceEvent sequences_stored[] = {};

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       singulars_stored,
                                       aggregates_stored,
                                       sequences_stored,
                                       1, 0, 0,
                                       &num_singulars_stored,
                                       &num_aggregates_stored,
                                       &num_sequences_stored,
                                       &capacity);

  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;
  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &singulars_drained,
                                                          &aggregates_drained,
                                                          &sequences_drained,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert_true (success);

  assert_singulars_equal_variants (singulars_stored, 1, singulars_drained);
  g_assert_cmpint (c_array_len (aggregates_drained), ==, 0);
  g_assert_cmpint (c_array_len (sequences_drained), ==, 0);

  trash_singular_event (singulars_stored);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
}

static void
test_persistent_cache_drain_one_aggregate_event_succeeds (gboolean     *unused,
                                                          gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  SingularEvent singulars_stored[] = {};

  AggregateEvent aggregates_stored[1];
  make_aggregate_event (1, aggregates_stored);
  aggregate_event_own (aggregates_stored);

  SequenceEvent sequences_stored[] = {};

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       singulars_stored,
                                       aggregates_stored,
                                       sequences_stored,
                                       0, 1, 0,
                                       &num_singulars_stored,
                                       &num_aggregates_stored,
                                       &num_sequences_stored,
                                       &capacity);

  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;
  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &singulars_drained,
                                                          &aggregates_drained,
                                                          &sequences_drained,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert_true (success);

  g_assert_cmpint (c_array_len (singulars_drained), ==, 0);
  assert_aggregates_equal_variants (aggregates_stored, 1, aggregates_drained);
  g_assert_cmpint (c_array_len (sequences_drained), ==, 0);

  trash_aggregate_event (aggregates_stored);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
}

static void
test_persistent_cache_drain_one_sequence_event_succeeds (gboolean     *unused,
                                                         gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  SingularEvent singulars_stored[] = {};
  AggregateEvent aggregates_stored[] = {};

  SequenceEvent sequences_stored[1];
  make_sequence_event (1, sequences_stored);
  sequence_event_own (sequences_stored);

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       singulars_stored,
                                       aggregates_stored,
                                       sequences_stored,
                                       0, 0, 1,
                                       &num_singulars_stored,
                                       &num_aggregates_stored,
                                       &num_sequences_stored,
                                       &capacity);

  g_assert_cmpint (num_sequences_stored, ==, 1);

  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;

  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &singulars_drained,
                                                          &aggregates_drained,
                                                          &sequences_drained,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert_true (success);

  g_assert_cmpint (c_array_len (singulars_drained), ==, 0);
  g_assert_cmpint (c_array_len (aggregates_drained), ==, 0);
  assert_sequences_equal_variants (sequences_stored, 1, sequences_drained);

  trash_sequence_event (sequences_stored);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
}

static void
test_persistent_cache_drain_many_succeeds (gboolean     *unused,
                                           gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  // Fill it up first.
  SingularEvent *singulars_stored;
  AggregateEvent *aggregates_stored;
  SequenceEvent *sequences_stored;

  gint num_singulars_made, num_aggregates_made, num_sequences_made;

  make_many_events (&singulars_stored,
                    &aggregates_stored,
                    &sequences_stored,
                    &num_singulars_made,
                    &num_aggregates_made,
                    &num_sequences_made);

  singular_buffer_own (singulars_stored, num_singulars_made);
  aggregate_buffer_own (aggregates_stored, num_aggregates_made);
  sequence_buffer_own (sequences_stored, num_sequences_made);

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       singulars_stored,
                                       aggregates_stored,
                                       sequences_stored,
                                       num_singulars_made,
                                       num_aggregates_made,
                                       num_sequences_made,
                                       &num_singulars_stored,
                                       &num_aggregates_stored,
                                       &num_sequences_stored,
                                       &capacity);

  // Check if we get the same things back.
  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;
  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &singulars_drained,
                                                          &aggregates_drained,
                                                          &sequences_drained,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert_true (success);
  assert_singulars_equal_variants (singulars_stored, num_singulars_made,
                                   singulars_drained);
  assert_aggregates_equal_variants (aggregates_stored, num_aggregates_made,
                                    aggregates_drained);
  assert_sequences_equal_variants (sequences_stored, num_sequences_made,
                                   sequences_drained);

  free_singular_buffer (singulars_stored, num_singulars_made);
  free_aggregate_buffer (aggregates_stored, num_aggregates_made);
  free_sequence_buffer (sequences_stored, num_sequences_made);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
}

static void
test_persistent_cache_drain_empty_succeeds (gboolean     *unused,
                                            gconstpointer dontuseme)
{
  // Don't store anything.
  EmerPersistentCache *cache = make_testing_cache ();
  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;
  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &singulars_drained,
                                                          &aggregates_drained,
                                                          &sequences_drained,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert_true (success);

  g_assert_cmpint (c_array_len (singulars_drained), ==, 0);
  g_assert_cmpint (c_array_len (aggregates_drained), ==, 0);
  g_assert_cmpint (c_array_len (sequences_drained), ==, 0);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
}

static void
test_persistent_cache_purges_when_out_of_date_succeeds (gboolean     *unused,
                                                        gconstpointer dontuseme)
{
  GError *error = NULL;
  EmerCacheSizeProvider *cache_size_provider =
    emer_cache_size_provider_new_full (TEST_DIRECTORY TEST_CACHE_SIZE_FILE);
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new_full (TEST_DIRECTORY TEST_CACHE_VERSION_FILE);
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (NULL, &error, TEST_DIRECTORY,
                                    cache_size_provider, boot_id_provider,
                                    cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL);
  g_assert_no_error (error);

  g_object_unref (cache_size_provider);
  g_object_unref (boot_id_provider);

  gint num_singulars_made, num_aggregates_made, num_sequences_made;
  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;

  store_many (cache, &num_singulars_made, &num_aggregates_made,
              &num_sequences_made, &num_singulars_stored,
              &num_aggregates_stored, &num_sequences_stored, &capacity);

  gint current_version;
  gboolean get_succeeded =
    emer_cache_version_provider_get_version (cache_version_provider,
                                             &current_version);
  g_assert_true (get_succeeded);

  gboolean set_succeeded =
    emer_cache_version_provider_set_version (cache_version_provider,
                                             current_version - 1, &error);
  g_assert_true (set_succeeded);

  g_assert_no_error (error);
  g_object_unref (cache_version_provider);
  g_object_unref (cache);

  EmerPersistentCache *cache2 = make_testing_cache ();

  // Metrics should all be purged now.
  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;
  emer_persistent_cache_drain_metrics (cache2,
                                       &singulars_drained,
                                       &aggregates_drained,
                                       &sequences_drained,
                                       MAX_BYTES_TO_READ);
  g_object_unref (cache2);

  g_assert_cmpint (c_array_len (singulars_drained), ==, 0);
  g_assert_cmpint (c_array_len (aggregates_drained), ==, 0);
  g_assert_cmpint (c_array_len (sequences_drained), ==, 0);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
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
  EmerPersistentCache *cache = make_testing_cache ();

  GError *error = NULL;
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, TRUE);
  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  gint64 first_offset = read_offset ();

  gint64 absolute_time, relative_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time));
  g_assert_true (emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time));

  get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, TRUE);
  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  g_assert_true (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset();

  // The offset should not have changed.
  g_assert_cmpint (first_offset, ==, second_offset);
  g_assert_true (boot_offset_was_reset ());

  g_object_unref (cache);
}

/*
 * Triggers the computation of a new boot offset by storing events with no
 * preexisting boot metadata file, which resets the offset to 0. Then unrefs the
 * persistent cache and makes it anew, clearing its in-memory cache. Then
 * mutates the metadata file to simulate a new boot without modifying the
 * timestamps, prompting the persistent cache to compute a new boot offset that
 * should be approximately 0.
 */
static void
test_persistent_cache_computes_reasonable_offset (gboolean     *unused,
                                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  store_single_singular_event (cache, &capacity);

  gint64 first_offset = read_offset ();
  g_assert_true (boot_offset_was_reset ());

  gint64 absolute_time, relative_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time));
  g_assert_true (emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time));

  g_object_unref (cache);
  EmerPersistentCache *cache2 = make_testing_cache ();

  // Mutate boot id directly because we cannot actually reboot in a test case.
  set_boot_id_in_metadata_file (FAKE_BOOT_ID);

  store_single_aggregate_event (cache2, &capacity);

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

  GError *error = NULL;
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, TRUE);
  g_assert_true (get_succeeded);
  g_assert_no_error (error);
  g_assert_true (boot_offset_was_reset ());

  gint64 relative_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time));

  gint64 absolute_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time));

  g_object_unref (cache);
  set_boot_id_in_metadata_file (FAKE_BOOT_ID);

  EmerPersistentCache *cache2 = make_testing_cache ();

  // This call should have to compute the boot offset itself.
  get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache2, NULL, &error, TRUE);

  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  g_assert_true (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset ();

  // This should not have simply reset the metadata file again.
  g_assert_false (boot_offset_was_reset ());

  g_object_unref (cache2);
  EmerPersistentCache *cache3 = make_testing_cache ();

  get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache3, NULL, &error, TRUE);
  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  gint64 third_offset = read_offset ();
  g_assert_cmpint (third_offset, ==, second_offset);

  g_object_unref (cache3);
}

/*
 * Creates a default boot metadata file and stores a single event. Then corrupts
 * the metadata file by removing the offset from it. Finally, stores another
 * event, prompting the persistent cache to detect the corruption and purge the
 * existing event but not the one being stored.
 */
static void
test_persistent_cache_wipes_metrics_when_boot_offset_corrupted (gboolean     *unused,
                                                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  write_default_boot_offset_key_file_to_disk ();

  capacity_t capacity;

  store_single_singular_event (cache, &capacity);

  // Clear in-memory boot offset cache.
  g_object_unref (cache);

  // Corrupt metadata file.
  remove_offset ();

  EmerPersistentCache *cache2 = make_testing_cache ();

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Could not find a "
                         "valid boot offset in the metadata file. Error: *.");

  /*
   * This call should detect corruption and wipe the persistent cache of all
   * previous events. However, this new aggregate event should be stored.
   */
  store_single_aggregate_event (cache2, &capacity);

  g_test_assert_expected_messages ();

  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;
  emer_persistent_cache_drain_metrics (cache2, &singulars_drained,
                                       &aggregates_drained, &sequences_drained,
                                       MAX_BYTES_TO_READ);

  // Only an aggregate event should remain.
  g_assert_cmpint (c_array_len (singulars_drained), ==, 0);
  g_assert_cmpint (c_array_len (aggregates_drained), ==, 1);
  g_assert_cmpint (c_array_len (sequences_drained), ==, 0);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);

  g_object_unref (cache2);
}

/*
 * Creates a default boot metadata file. Then corrupts the metadata file by
 * removing the offset from it. Finally, requests the boot offset, prompting the
 * persistent cache to detect the corruption and reset the metadata file.
 */
static void
test_persistent_cache_resets_boot_metadata_file_when_boot_offset_corrupted (gboolean     *unused,
                                                                            gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  write_default_boot_offset_key_file_to_disk ();

  // Corrupt metadata file.
  remove_offset ();

  GError *error = NULL;
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Could not find a "
                         "valid boot offset in the metadata file. Error: *.");

  // This call should detect corruption and reset the metadata file.
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, TRUE);

  g_assert_true (get_succeeded);

  g_test_assert_expected_messages ();
  g_assert_no_error (error);

  g_assert_true (boot_offset_was_reset ());

  g_object_unref (cache);
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
  write_default_boot_offset_key_file_to_disk ();

  gint64 first_offset;
  GError *error = NULL;
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, &first_offset, &error,
                                                TRUE);
  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  gint64 relative_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time));

  gint64 absolute_time;
  g_assert_true (emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time));

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
    emer_persistent_cache_get_boot_time_offset (cache, &second_offset, &error,
                                                TRUE);
  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  g_assert_true (boot_timestamp_is_valid (relative_time, absolute_time));

  g_assert_cmpint (first_offset, ==, second_offset);

  g_object_unref (cache);
}

/*
 * Ensures that a request for the boot time offset doesn't cause a write to the
 * metadata file if the 'always_update_timestamps' parameter is set to FALSE.
 * If the boot offset and/or the boot id needs to be changed, the timestamps
 * are updated regardless of the value of the parameter. To ensure neither of
 * these need to be changed, we make a request with the parameter set to TRUE
 * first, which corrects the default metadata file before we make the call we
 * really want to test, the one with the parameter set to FALSE.
 */
static void
test_persistent_cache_get_offset_wont_update_timestamps_if_it_isnt_supposed_to (gboolean     *unused,
                                                                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  write_default_boot_offset_key_file_to_disk ();

  GError *error = NULL;

  // Update metadata file to reasonable values.
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, TRUE);
  g_assert_true (get_succeeded);
  g_assert_no_error (error);
  gint64 relative_time = read_relative_time ();
  gint64 absolute_time = read_absolute_time ();

  // Make a little time pass.
  g_usleep (75000); // 0.075 seconds

  // This call shouldn't update the metadata file.
  get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, FALSE);

  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  // These timestamps should not have changed.
  g_assert_cmpint (relative_time, ==, read_relative_time ());
  g_assert_cmpint (absolute_time, ==, read_absolute_time ());
  g_object_unref (cache);
}

/*
 * Ensures that a request for the boot time offset updates the timestamps in the
 * boot offset metadata file when the 'always_update_timestamps' parameter is
 * TRUE. If the boot offset and/or the boot id needs to be changed, the
 * timestamps are updated regardless of the value of the parameter. To ensure
 * neither of these need to be changed, we make an initial request for the boot
 * time offset that corrects the default metadata file before we make the call
 * we really want to test.
 */
static void
test_persistent_cache_get_offset_updates_timestamps_when_requested (gboolean     *unused,
                                                                    gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  write_default_boot_offset_key_file_to_disk ();

  GError *error = NULL;

  // Update metadata file to reasonable values.
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, FALSE);
  g_assert_true (get_succeeded);

  g_assert_no_error (error);
  gint64 relative_time = read_relative_time ();
  gint64 absolute_time = read_absolute_time ();

  // Make a little time pass.
  g_usleep (75000); // 0.075 seconds

  // This call should update the timestamps in the metadata file.
  get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, TRUE);
  g_assert_true (get_succeeded);

  g_assert_no_error (error);

  // These timestamps should have increased.
  g_assert_cmpint (relative_time, <, read_relative_time ());
  g_assert_cmpint (absolute_time, <, read_absolute_time ());
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
  write_default_boot_offset_key_file_to_disk ();

  GError *error = NULL;

  // Update metadata file to reasonable values.
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, TRUE);

  g_assert_true (get_succeeded);
  g_assert_no_error (error);

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

  /*
   * This call should create the metadata file even though the
   * always_update_timestamps parameter is FALSE.
   */
  gboolean get_succeeded =
    emer_persistent_cache_get_boot_time_offset (cache, NULL, &error, FALSE);

  g_assert_true (get_succeeded);
  g_assert_no_error (error);

  // The previous request should have reset the metadata file.
  g_assert_true (boot_offset_was_reset ());

  g_object_unref (cache);
}

// Tests for required libraries:

/*
 * We include this test because we are relying on this function returning zero
 * when an empty file is read to indicate we don't need to overwrite an empty
 * file. This will preserve some lifetime on our users' storage.
 */
static void
test_g_file_measure_disk_usage_returns_zero_on_empty_file (gboolean     *unused,
                                                           gconstpointer dontuseme)
{
  gchar *path = g_strconcat (TEST_DIRECTORY, "empty_file", NULL);
  GFile *file = g_file_new_for_path (path);

  GError *error = NULL;
  g_file_replace_contents (file, "", 0, NULL, FALSE,
                           G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL,
                           &error);
  g_assert_no_error (error);

  guint64 disk_usage = 555; // Arbitrary non-zero value
  gboolean measure_succeeded =
    g_file_measure_disk_usage (file, G_FILE_MEASURE_REPORT_ANY_ERROR, NULL,
                               NULL, NULL, &disk_usage, NULL, NULL, &error);
  g_assert_true (measure_succeeded);
  g_assert_no_error (error);
  g_assert_cmpint (disk_usage, ==, 0);

  g_object_unref (file);
  g_unlink (path);
  g_free (path);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

// We are using a gboolean as a fixture type, but it will go unused.
#define ADD_CACHE_TEST_FUNC(path, func) \
  g_test_add((path), gboolean, NULL, setup, (func), teardown)

  ADD_CACHE_TEST_FUNC ("/persistent-cache/new-succeeds",
                       test_persistent_cache_new_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-sets-out-parameters",
                       test_persistent_cache_store_sets_out_parameters);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-one-singular-event-succeeds",
                       test_persistent_cache_store_one_singular_event_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-one-aggregate-event-succeeds",
                       test_persistent_cache_store_one_aggregate_event_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-one-sequence-event-succeeds",
                       test_persistent_cache_store_one_sequence_event_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-one-of-each-succeeds",
                       test_persistent_cache_store_one_of_each_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-many-succeeds",
                       test_persistent_cache_store_many_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-when-full-succeeds",
                       test_persistent_cache_store_when_full_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-one-singular-event-succeeds",
                       test_persistent_cache_drain_one_singular_event_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-one-aggregate-event-succeeds",
                       test_persistent_cache_drain_one_aggregate_event_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-one-sequence-event-succeeds",
                       test_persistent_cache_drain_one_sequence_event_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-many-succeeds",
                       test_persistent_cache_drain_many_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-empty-succeeds",
                       test_persistent_cache_drain_empty_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/purges-when-out-of-date-succeeds",
                       test_persistent_cache_purges_when_out_of_date_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/builds-boot-metadata-file",
                       test_persistent_cache_builds_boot_metadata_file);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/computes-reasonable-offset",
                       test_persistent_cache_computes_reasonable_offset);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/does-not-compute-offset-when-boot-id-is-same",
                       test_persistent_cache_does_not_compute_offset_when_boot_id_is_same);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/wipes-metrics-when-boot-offset-corrupted",
                       test_persistent_cache_wipes_metrics_when_boot_offset_corrupted);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/resets-boot-metadata-file-when-boot-offset-corrupted",
                       test_persistent_cache_resets_boot_metadata_file_when_boot_offset_corrupted);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/reads-cached-boot-offset",
                       test_persistent_cache_reads_cached_boot_offset);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/get-offset-wont-update-timestamps-if-it-isnt-supposed-to",
                       test_persistent_cache_get_offset_wont_update_timestamps_if_it_isnt_supposed_to);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/get-offset-updates-timestamps-when-requested",
                       test_persistent_cache_get_offset_updates_timestamps_when_requested);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/updates-timestamps-on-finalize",
                       test_persistent_cache_updates_timestamps_on_finalize);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/get-offset-can-build-boot-metadata-file",
                       test_persistent_cache_get_offset_can_build_boot_metadata_file);
  ADD_CACHE_TEST_FUNC ("/g-file/measure-disk-usage-returns-zero-on-empty-file",
                       test_g_file_measure_disk_usage_returns_zero_on_empty_file);
#undef ADD_CACHE_TEST_FUNC

  return g_test_run ();
}
