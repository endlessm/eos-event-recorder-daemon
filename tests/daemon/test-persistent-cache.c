/* Copyright 2014 Endless Mobile, Inc. */

#include "daemon/emer-persistent-cache.h"

#include <glib.h>
#include <stdio.h>
#include <glib/gstdio.h>

#include "shared/metrics-util.h"

#define TEST_DIRECTORY "/tmp/metrics_testing/"

#define TEST_SYSTEM_BOOT_ID_FILE "system_boot_id_file"
#define TEST_CACHE_VERSION_FILE "local_version_file"

// Generated via uuidgen.
#define FAKE_SYSTEM_BOOT_ID "1ca14ab8-bed6-4bc0-8369-484518d22a31\n"
#define FAKE_BOOT_ID "baccd4dd-9765-4eb2-a2a0-03c6623471e6\n"
#define FAKE_BOOT_OFFSET 4000000000 // 4 seconds

#define TEST_UPDATE_OFFSET_INTERVAL (60u * 60u) // 1 hour
#define TEST_SIZE 1024000u

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

#define NANOSECONDS_PER_MILLISECOND 1000000

#define ACCEPTABLE_OFFSET_VARIANCE (500 /* Milliseconds */ * NANOSECONDS_PER_MILLISECOND)

#define DEFAULT_BOOT_OFFSET_KEY_FILE_DATA \
  "[time]\n" \
  "boot_offset=0\n" \
  "was_reset=true\n" \
  "absolute_time=1403195800943262692\n" \
  "relative_time=2516952859775\n" \
  "boot_id=299a89b4-72c2-455a-b2d3-13c1a7c8c11f\n"

#define DEFAULT_CACHE_VERSION_KEY_FILE_DATA \
  "[cache_version_info]\n" \
  "version=2\n"

// ---- Helper functions come first ----

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
write_default_cache_version_key_file_to_disk (void)
{
  GKeyFile *key_file = g_key_file_new ();
  g_assert (g_key_file_load_from_data (key_file,
                                       DEFAULT_CACHE_VERSION_KEY_FILE_DATA,
                                       -1, G_KEY_FILE_NONE, NULL));

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY TEST_CACHE_VERSION_FILE,
                                     NULL));
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
  g_unlink (TEST_DIRECTORY BOOT_OFFSET_METAFILE);
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
  write_mock_system_boot_id_file ();
  write_default_cache_version_key_file_to_disk ();
  write_empty_metrics_files ();
}

static EmerPersistentCache *
make_testing_cache (void)
{
  GError *error = NULL;
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new_full (TEST_DIRECTORY TEST_CACHE_VERSION_FILE);
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (NULL, &error, TEST_DIRECTORY, TEST_SIZE,
                                    boot_id_provider, cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL);
  g_object_unref (boot_id_provider);
  g_object_unref (cache_version_provider);
  g_assert_no_error (error);
  return cache;
}

/*
 * Returns a new GKeyFile associated with the boot timing metafile.
 * Keyfile should be unref'd via g_key_file_unref().
 */
static GKeyFile *
load_testing_key_file (void)
{
  GKeyFile *key_file = g_key_file_new ();
  gchar *full_path = g_strconcat (TEST_DIRECTORY, BOOT_OFFSET_METAFILE, NULL);
  g_assert (g_key_file_load_from_file (key_file, full_path, G_KEY_FILE_NONE,
                                       NULL));
  return key_file;
}

/*
 * Will overwrite the contents of the boot id metafile's boot offset with a
 * given new_offset.
 */
static void
set_boot_offset_in_metafile (gint64 new_offset)
{
  GKeyFile *key_file = load_testing_key_file ();

  g_key_file_set_int64 (key_file,
                        CACHE_TIMING_GROUP_NAME,
                        CACHE_BOOT_OFFSET_KEY,
                        new_offset);

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY BOOT_OFFSET_METAFILE,
                                     NULL));
  g_key_file_unref (key_file);
}

/*
 * Will populate the boot metafile with data similar to the default that will be
 * written when the cache and boot metafile are reset to defaults. Must be
 * called AFTER the testing directory exists (after a Persistent Cache instance
 * has been new'd constructed.)
 */
static void
write_default_boot_offset_key_file_to_disk (void)
{
  GKeyFile *key_file = g_key_file_new ();
  g_assert (g_key_file_load_from_data (key_file,
                                       DEFAULT_BOOT_OFFSET_KEY_FILE_DATA,
                                       -1, G_KEY_FILE_NONE, NULL));

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY BOOT_OFFSET_METAFILE,
                                     NULL));
}

/*
 * Will overwrite the contents of the boot id metafile's boot id with a
 * given new_boot_id.
 */
static void
set_boot_id_in_metafile (gchar *new_boot_id)
{
  GKeyFile *key_file = load_testing_key_file ();

  g_key_file_set_string (key_file,
                         CACHE_TIMING_GROUP_NAME,
                         CACHE_LAST_BOOT_ID_KEY,
                         new_boot_id);

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY BOOT_OFFSET_METAFILE,
                                     NULL));
  g_key_file_unref (key_file);
}

/*
 * Removes the offset key/value pair from the boot metafile to simulate
 * corruption and writes that change to disk.
 */
static void
remove_offset (void)
{
  GKeyFile *key_file = load_testing_key_file ();
  g_assert (g_key_file_remove_key (key_file, CACHE_TIMING_GROUP_NAME,
                                   CACHE_BOOT_OFFSET_KEY, NULL));

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY BOOT_OFFSET_METAFILE,
                                     NULL));
  g_key_file_unref (key_file);
}

/*
 * Gets the stored offset from disk (metafile.)
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
  g_assert (error == NULL);
  return stored_offset;
}

/*
 * Gets the stored metafile was reset flag from disk (metafile.)
 * Also ensures the reset value is 0.
 */
static gboolean
read_whether_boot_offset_is_reset_value (void)
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
 * Gets the stored relative time from disk (metafile.)
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
 * Gets the stored absolute time from disk (metafile.)
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
 * Will perform a disk lookup of the metafile to see if the stored timestamps
 * are greater than or equal to the previous timestamps (given as parameters)
 * and less than or equal to timestamps generated by subsequently generated
 * timestamps.
 */
static gboolean
boot_timestamp_is_valid (gint64 previous_relative_timestamp,
                         gint64 previous_absolute_timestamp)
{
  gint64 stored_relative_timestamp = read_relative_time ();
  gint64 stored_absolute_timestamp = read_absolute_time ();

  gint64 after_relative_timestamp, after_absolute_timestamp;
  g_assert (get_current_time (CLOCK_BOOTTIME, &after_relative_timestamp) &&
            get_current_time (CLOCK_REALTIME, &after_absolute_timestamp));

  // The actual testing:
  return (previous_relative_timestamp <= stored_relative_timestamp &&
          stored_relative_timestamp   <= after_relative_timestamp  &&
          previous_absolute_timestamp <= stored_absolute_timestamp &&
          stored_absolute_timestamp   <= after_absolute_timestamp);
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

  g_assert (num_singulars_stored == 1);
  g_assert (num_aggregates_stored == 0);
  g_assert (num_sequences_stored == 0);

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

  g_assert (num_singulars_stored == 0);
  g_assert (num_aggregates_stored == 1);
  g_assert (num_sequences_stored == 0);

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

  g_assert (num_singulars_stored == 0);
  g_assert (num_aggregates_stored == 0);
  g_assert (num_sequences_stored == 1);

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
      g_assert (i < singular_array_length && variants[i] != NULL);

      GVariant *singular_variant = singular_to_variant (singular_array + i);
      g_assert (g_variant_equal (singular_variant, variants[i]));
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
      g_assert (i < aggregate_array_length && variants[i] != NULL);

      GVariant *aggregate_variant = aggregate_to_variant (aggregate_array + i);
      g_assert (g_variant_equal (aggregate_variant, variants[i]));
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
      g_assert (i < sequence_array_length && variants[i] != NULL);

      GVariant *sequence_variant = sequence_to_variant (sequence_array + i);
      g_assert (g_variant_equal (sequence_variant, variants[i]));
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
  g_assert (cache != NULL);
  g_object_unref (cache);
  if (error != NULL)
    g_error_free (error);
  g_assert (error == NULL);
}

static void
test_persistent_cache_store_one_singular_event_succeeds (gboolean     *unused,
                                                         gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  gboolean success = store_single_singular_event (cache, &capacity);
  g_object_unref (cache);
  g_assert (success);
  g_assert (capacity == CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_aggregate_event_succeeds (gboolean     *unused,
                                                          gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  gboolean success = store_single_aggregate_event (cache, &capacity);
  g_object_unref (cache);
  g_assert (success);
  g_assert (capacity == CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_sequence_event_succeeds (gboolean     *unused,
                                                         gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  gboolean success = store_single_sequence_event (cache, &capacity);
  g_object_unref (cache);
  g_assert (success);
  g_assert (capacity == CAPACITY_LOW);
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

  g_assert (success);

  g_assert (num_singulars_stored == 1);
  g_assert (num_aggregates_stored == 1);
  g_assert (num_sequences_stored == 1);

  g_assert (capacity == CAPACITY_LOW);
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
  g_assert (success);

  g_assert (num_singulars_stored == num_singulars_made);
  g_assert (num_aggregates_stored == num_aggregates_made);
  g_assert (num_sequences_stored == num_sequences_made);
}

static void
test_persistent_cache_store_when_full_succeeds (gboolean     *unused,
                                                gconstpointer dontuseme)
{
  guint64 space_in_bytes = 3000u;
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new_full (TEST_DIRECTORY TEST_CACHE_VERSION_FILE);
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (NULL, NULL, TEST_DIRECTORY, space_in_bytes,
                                    boot_id_provider, cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL);

  g_object_unref (cache_version_provider);
  g_object_unref (boot_id_provider);

  capacity_t capacity = CAPACITY_LOW;

  // Store a ton, until it is full.
  // TODO: Find a less hacky way of doing this.
  gint iterations = space_in_bytes / 150;
  for (gint i = 0; i < iterations; i++)
    {
      gint num_singulars_made, num_aggregates_made, num_sequences_made;
      gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
      gboolean success =
        store_many (cache, &num_singulars_made, &num_aggregates_made,
                    &num_sequences_made, &num_singulars_stored,
                    &num_aggregates_stored, &num_sequences_stored, &capacity);
      g_assert (success);

      if (capacity == CAPACITY_MAX)
        {
          g_assert (num_singulars_stored <= num_singulars_made);
          g_assert (num_aggregates_stored <= num_aggregates_made);
          g_assert (num_sequences_stored <= num_sequences_made);
          break;
        }

      g_assert (num_singulars_stored == num_singulars_made);
      g_assert (num_aggregates_stored == num_aggregates_made);
      g_assert (num_sequences_stored == num_sequences_made);
    }

  g_object_unref (cache);
  g_assert (capacity == CAPACITY_MAX);
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

  g_assert (success);

  assert_singulars_equal_variants (singulars_stored, 1, singulars_drained);
  g_assert (c_array_len (aggregates_drained) == 0);
  g_assert (c_array_len (sequences_drained) == 0);

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

  g_assert (success);

  g_assert (c_array_len (singulars_drained) == 0);
  assert_aggregates_equal_variants (aggregates_stored, 1, aggregates_drained);
  g_assert (c_array_len (sequences_drained) == 0);

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

  g_assert (num_sequences_stored == 1);

  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;

  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &singulars_drained,
                                                          &aggregates_drained,
                                                          &sequences_drained,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert (success);

  g_assert (c_array_len (singulars_drained) == 0);
  g_assert (c_array_len (aggregates_drained) == 0);
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

  g_assert (success);
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

  g_assert (success);

  g_assert (c_array_len (singulars_drained) == 0);
  g_assert (c_array_len (aggregates_drained) == 0);
  g_assert (c_array_len (sequences_drained) == 0);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
}

static void
test_persistent_cache_purges_when_out_of_date_succeeds (gboolean     *unused,
                                                        gconstpointer dontuseme)
{
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new_full (TEST_DIRECTORY TEST_CACHE_VERSION_FILE);
  EmerPersistentCache *cache =
    emer_persistent_cache_new_full (NULL, NULL, TEST_DIRECTORY, TEST_SIZE,
                                    boot_id_provider, cache_version_provider,
                                    TEST_UPDATE_OFFSET_INTERVAL);

  g_object_unref (boot_id_provider);

  gint num_singulars_made, num_aggregates_made, num_sequences_made;
  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;

  store_many (cache, &num_singulars_made, &num_aggregates_made,
              &num_sequences_made, &num_singulars_stored,
              &num_aggregates_stored, &num_sequences_stored, &capacity);

  gint current_version;
  g_assert (emer_cache_version_provider_get_version (cache_version_provider,
                                                     &current_version));
  GError *error = NULL;
  g_assert (emer_cache_version_provider_set_version (cache_version_provider,
                                                     current_version - 1,
                                                     &error));
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

  g_assert (c_array_len (singulars_drained) == 0);
  g_assert (c_array_len (aggregates_drained) == 0);
  g_assert (c_array_len (sequences_drained) == 0);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);
}

/*
 * Test creates an default boot metafile. A single metric is added. Then it
 * corrupts the metafile by removing the offset from it. Finally, a store call
 * is made again to detect the corruption and purge the old metrics, but store
 * the metrics sent by the latest *_store_metrics() call.
 */
static void
test_persistent_cache_wipes_metrics_when_boot_offset_corrupted (gboolean     *unused,
                                                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  write_default_boot_offset_key_file_to_disk ();

  capacity_t capacity;

  // Insert a metric.
  store_single_singular_event (cache, &capacity);

  // Corrupt metafile.
  remove_offset ();

  // Reset cached metadata.
  g_object_unref (cache);
  EmerPersistentCache *cache2 = make_testing_cache ();

  // This call should detect corruption and wipe the cache of all previous
  // events. However, this new aggregate event should be stored.
  store_single_aggregate_event (cache2, &capacity);

  GVariant **singulars_drained, **aggregates_drained, **sequences_drained;
  emer_persistent_cache_drain_metrics (cache2, &singulars_drained,
                                       &aggregates_drained, &sequences_drained,
                                       MAX_BYTES_TO_READ);

  // Only an aggregate event should remain.
  g_assert (c_array_len (singulars_drained) == 0);
  g_assert (c_array_len (aggregates_drained) == 1);
  g_assert (c_array_len (sequences_drained) == 0);

  free_variant_c_array (singulars_drained);
  free_variant_c_array (aggregates_drained);
  free_variant_c_array (sequences_drained);

  g_object_unref (cache2);
}

/*
 * Test creates an default boot metafile. Then it corrupts the metafile by
 * removing the offset from it. Finally, a request for the offset is made to
 * detect the corruption and reset the metafile.
 */
static void
test_persistent_cache_resets_boot_metafile_when_boot_offset_corrupted (gboolean     *unused,
                                                                       gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  write_default_boot_offset_key_file_to_disk ();

  // Corrupt metafile.
  remove_offset ();

  // This call should detect corruption and reset the metafile.
  gint64 unused_offset;
  GError *error = NULL;
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &unused_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);

  g_assert (read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache);
}

/*
 * Test triggers the computation of a new boot offset by asking for the new boot
 * offset with no preexisting boot metafile, which triggers a reset to offset 0
 * and the saved boot id to the current boot id on the system. The persistent
 * cache is then unref'd and made anew. This causes the cached values to be
 * lost. The metafile must then be mutated to simulate a new boot. Then another
 * request for the boot offset is needed to get the cache to compute a new
 * offset. Then we need to unref this and create it AGAIN to remove the cached
 * values. Finally, one more request should write new timestamps but shouldn't
 * have a different offset as it should not be computed again in this case.
 *
 * Thus, if you restart the cache with a preexisting cache from a previous boot,
 * then the relative time offsets will be the same.
 */
static void
test_persistent_cache_does_not_compute_offset_when_boot_id_is_same (gboolean     *unused,
                                                                    gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  gint64 unused_offset;
  GError *error = NULL;
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &unused_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);
  g_assert (read_whether_boot_offset_is_reset_value ());

  gint64 absolute_time, relative_time;
  g_assert (get_current_time (CLOCK_BOOTTIME, &relative_time) &&
            get_current_time (CLOCK_REALTIME, &absolute_time));

  g_object_unref (cache);
  set_boot_id_in_metafile (FAKE_BOOT_ID);

  EmerPersistentCache *cache2 = make_testing_cache ();

  // This call should have to compute the boot offset itself.
  g_assert (emer_persistent_cache_get_boot_time_offset (cache2, &unused_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);

  g_assert (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset ();

  // This should not have simply reset the metafile again.
  g_assert_false (read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache2);
  EmerPersistentCache *cache3 = make_testing_cache ();

  g_assert (emer_persistent_cache_get_boot_time_offset (cache3, &unused_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);

  gint64 third_offset = read_offset ();
  g_assert (third_offset == second_offset);

  g_object_unref (cache3);
}

/*
 * Test triggers the computation of a new boot offset by storing metrics with no
 * preexisting boot metafile, which triggers a reset to offset 0. The persistent
 * cache is then unref'd and made anew. This causes the cached value to be lost.
 * Then the test mutates the boot id stored from the previous metrics storing
 * call which will make the persistent cache believe this is a different boot
 * than before.
 *
 * The validity of the two offsets can really only be approximated by a
 * #define'd acceptable variance.
 */
static void
test_persistent_cache_computes_reasonable_offset (gboolean     *unused,
                                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  store_single_singular_event (cache, &capacity);

  gint64 first_offset = read_offset ();
  g_assert (read_whether_boot_offset_is_reset_value ());

  gint64 absolute_time, relative_time;
  g_assert (get_current_time (CLOCK_BOOTTIME, &relative_time) &&
            get_current_time (CLOCK_REALTIME, &absolute_time));

  g_object_unref (cache);
  EmerPersistentCache *cache2 = make_testing_cache ();

  // Mutate boot id externally because we cannot actually reboot in a test case.
  set_boot_id_in_metafile (FAKE_BOOT_ID);

  store_single_aggregate_event (cache2, &capacity);

  g_assert (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset ();
  g_assert (second_offset <= first_offset + ACCEPTABLE_OFFSET_VARIANCE);
  g_assert (second_offset >= first_offset - ACCEPTABLE_OFFSET_VARIANCE);

  // This should not have simply reset the metafile again.
  g_assert (!read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache2);
}

/*
 * This test is ensuring that we can build a metafile from nothing
 * should one not be found.
 *
 * Will test if the cached offset loading doesn't crash or produce unexpected
 * values by making several requests for the boot offset without initializing
 * the metafile or cleaning it up between requests.
 *
 * This test does no special mutation of the metafile in the test case beyond
 * what the production code would normally do. Thus the offset will always be
 * reset to, and then cached to, 0.
 */
static void
test_persistent_cache_builds_boot_metafile (gboolean     *unused,
                                            gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  GError *error = NULL;
  gint64 unused_offset;
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &unused_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);

  gint64 first_offset = read_offset ();
  gint64 absolute_time, relative_time;
  g_assert (get_current_time (CLOCK_BOOTTIME, &relative_time) &&
            get_current_time (CLOCK_REALTIME, &absolute_time));

  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &unused_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);

  g_assert (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset();

  // The offset should not have changed.
  g_assert (first_offset == second_offset);
  g_assert (read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache);
}

/*
 * Tests if the cached value is read when present, by changing what is on disk
 * to a different value in between calls to the same cache.
 */
static void
test_persistent_cache_reads_cached_boot_offset (gboolean     *unused,
                                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  write_default_boot_offset_key_file_to_disk ();

  gint64 first_offset;
  GError *error = NULL;
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &first_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);

  gint64 absolute_time, relative_time;
  g_assert (get_current_time (CLOCK_BOOTTIME, &relative_time) &&
            get_current_time (CLOCK_REALTIME, &absolute_time));

  // This value should never be read because the persistent cache should read
  // from its cached value next call.
  set_boot_offset_in_metafile (FAKE_BOOT_OFFSET);

  gint64 second_offset;

  // This call should read the offset from its cached value, not the new one
  // from disk.
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &second_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);

  g_assert (boot_timestamp_is_valid (relative_time, absolute_time));

  g_assert (first_offset == second_offset);

  g_object_unref (cache);
}

/*
 * This test ensures the request for the boot time offset won't cause a write to
 * metafile if the 'always_update_timestamps' boolean is set to FALSE. This
 * should always be the case unless the boot offset and/or the boot id needs to
 * be changed. To ensure it doesn't need to be changed, we make a request with
 * the boolen set to TRUE first which corrects the default metafile before we
 * make the call we really want to test -- the one with the boolean set to
 * FALSE.
 */
static void
test_persistent_cache_get_offset_wont_update_timestamps_if_it_isnt_supposed_to (gboolean     *unused,
                                                                                gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  write_default_boot_offset_key_file_to_disk ();

  gint64 unused_offset;
  GError *error = NULL;

  // Update to reasonable values.
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &unused_offset,
                                                        &error, TRUE));
  g_assert_no_error (error);
  gint64 rel_time = read_relative_time ();
  gint64 abs_time = read_absolute_time ();

  // Let's make a little time pass.
  g_usleep (75000); // 0.075 seconds

  // This call shouldn't update the metafile.
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &unused_offset,
                                                        &error, FALSE));
  g_assert_no_error (error);

   // These timestamps should not have changed.
   g_assert (rel_time == read_relative_time ());
   g_assert (abs_time == read_absolute_time ());
   g_object_unref (cache);
}

/*
 * This test ensures the get_boot_time_offset call will reset the metafile if
 * it needs to.  That is, when the metafile doesn't exist, is corrupted, or
 * the boot id doesn't match the system boot id. In this case, it is sufficient
 * to check that it creates a new metafile when one isn't found.
 */
static void
test_persistent_cache_get_offset_can_build_boot_metafile (gboolean     *unused,
                                                          gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  /* Don't write a default boot offset file, we want to create a new one via
     production code. */

  gint64 offset;
  GError *error = NULL;

  // This call should create the metafile even though the boolean is FALSE.
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &offset,
                                                        &error, FALSE));
  g_assert_no_error (error);

  // The previous request should have reset the metafile.
  g_assert (read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache);
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
  ADD_CACHE_TEST_FUNC ("/persistent-cache/builds-boot-metafile",
                       test_persistent_cache_builds_boot_metafile);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/computes-reasonable-offset",
                       test_persistent_cache_computes_reasonable_offset);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/does-not-compute-offset-when-boot-id-is-same",
                       test_persistent_cache_does_not_compute_offset_when_boot_id_is_same);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/resets-boot-metafile-when-boot-offset-corrupted",
                       test_persistent_cache_resets_boot_metafile_when_boot_offset_corrupted);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/wipes-metrics-when-boot-offset-corrupted",
                       test_persistent_cache_wipes_metrics_when_boot_offset_corrupted);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/reads-cached-boot-offset",
                       test_persistent_cache_reads_cached_boot_offset);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/get-offset-wont-update-timestamps-if-it-isnt-supposed-to",
                       test_persistent_cache_get_offset_wont_update_timestamps_if_it_isnt_supposed_to);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/get-offset-can-build-boot-metafile",
                       test_persistent_cache_get_offset_can_build_boot_metafile);
#undef ADD_CACHE_TEST_FUNC

  return g_test_run ();
}
