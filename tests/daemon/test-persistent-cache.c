/* Copyright 2014 Endless Mobile, Inc. */

#include "daemon/emer-persistent-cache.h"

#include <glib.h>
#include <stdio.h>
#include <glib/gstdio.h>

#include "shared/metrics-util.h"

#define TEST_DIRECTORY "/tmp/metrics_testing/"

#define TEST_SYSTEM_BOOT_ID_FILE "system_boot_id_file"

// Generated via uuidgen.
#define FAKE_SYSTEM_BOOT_ID "1ca14ab8-bed6-4bc0-8369-484518d22a31\n"
#define FAKE_BOOT_ID "baccd4dd-9765-4eb2-a2a0-03c6623471e6\n"
#define FAKE_RELATIVE_OFFSET 4000000000 // 4 seconds

#define TEST_SIZE 1024000

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

#define DEFAULT_KEY_FILE_DATA \
  "[time]\n" \
  "relative_time_offset=0\n" \
  "was_reset=true\n" \
  "absolute_time=1403195800943262692\n" \
  "relative_time=2516952859775\n" \
  "boot_id=299a89b4-72c2-455a-b2d3-13c1a7c8c11f\n"

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
teardown (gboolean     *unused,
          gconstpointer dontuseme)
{
  g_unlink (TEST_DIRECTORY CACHE_PREFIX INDIVIDUAL_SUFFIX);
  g_unlink (TEST_DIRECTORY CACHE_PREFIX AGGREGATE_SUFFIX);
  g_unlink (TEST_DIRECTORY CACHE_PREFIX SEQUENCE_SUFFIX);
  g_unlink (TEST_DIRECTORY CACHE_PREFIX LOCAL_CACHE_VERSION_METAFILE);
  g_unlink (TEST_DIRECTORY BOOT_TIMING_METAFILE);
}

static void
setup (gboolean     *unused,
       gconstpointer dontuseme)
{
  g_mkdir (TEST_DIRECTORY, 0777);
  teardown (unused, dontuseme);
  write_mock_system_boot_id_file ();
}

static EmerPersistentCache *
make_testing_cache (void)
{
  GError *error = NULL;
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          &error,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE,
                                                          boot_id_provider);
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
  gchar *full_path = g_strconcat (TEST_DIRECTORY, BOOT_TIMING_METAFILE, NULL);
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
                        CACHE_RELATIVE_OFFSET_KEY,
                        new_offset);

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY BOOT_TIMING_METAFILE,
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
write_default_key_file_to_disk (void)
{
  GKeyFile *key_file = g_key_file_new ();
  g_assert (g_key_file_load_from_data (key_file, DEFAULT_KEY_FILE_DATA, -1,
                                       G_KEY_FILE_NONE, NULL));

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY BOOT_TIMING_METAFILE,
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
                                     TEST_DIRECTORY BOOT_TIMING_METAFILE,
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
                                   CACHE_RELATIVE_OFFSET_KEY, NULL));

  g_assert (g_key_file_save_to_file (key_file,
                                     TEST_DIRECTORY BOOT_TIMING_METAFILE,
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
                                               CACHE_RELATIVE_OFFSET_KEY,
                                               &error);
  g_key_file_unref (key_file);
  g_assert (error == NULL);
  return stored_offset;
}

/*
 * Gets the stored metafile was reset flag from disk (metafile.)
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
  return was_reset;
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

static GVariant *
make_individual_event (gint choice)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("ay"));
  gint64 x;
  guint32 u;
  GVariant *mv;

  if (choice == 0)
    {
      u = 234u;
      g_variant_builder_add (&builder, "y", 0xde);
      g_variant_builder_add (&builder, "y", 0xad);
      g_variant_builder_add (&builder, "y", 0xbe);
      g_variant_builder_add (&builder, "y", 0xef);
      x = G_GINT64_CONSTANT (42);
      mv = g_variant_new_string ("murphy");
    }
  else if (choice == 1)
    {
      u = 121u;
      g_variant_builder_add (&builder, "y", 0x01);
      g_variant_builder_add (&builder, "y", 0x23);
      g_variant_builder_add (&builder, "y", 0x45);
      g_variant_builder_add (&builder, "y", 0x67);
      g_variant_builder_add (&builder, "y", 0x89);
      x = G_GINT64_CONSTANT (999);
      mv = g_variant_new_int32 (404);
    }
  else if (choice == 2)
    {
      u = 555u;
      g_variant_builder_add (&builder, "y", 0x4b);
      x = G_GINT64_CONSTANT (12012);
      mv = g_variant_new_string ("I am a banana!");
    }
  else if (choice == 3)
    {
      u = 411u;
      g_variant_builder_add (&builder, "y", 0x55);
      g_variant_builder_add (&builder, "y", 0x2c);
      x = G_GINT64_CONSTANT (-128);
      mv = g_variant_new_int32 (64);
    }
  else
    {
      g_error ("Tried to use a choice for make_individual_event that hasn't "
               "been programmed.");
    }

  return g_variant_new (INDIVIDUAL_TYPE, u, &builder, x, mv);
}

static GVariant *
make_aggregate_event (gint choice)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("ay"));
  gint64 x1;
  gint64 x2;
  GVariant *mv;
  guint32 u;

  if (choice == 0)
    {
      u = 12u;
      g_variant_builder_add (&builder, "y", 0xde);
      g_variant_builder_add (&builder, "y", 0xaf);
      g_variant_builder_add (&builder, "y", 0x00);
      g_variant_builder_add (&builder, "y", 0x01);
      x1 = G_GINT64_CONSTANT (9876);
      x2 = G_GINT64_CONSTANT (111);
      mv = g_variant_new_string ("meepo");
    }
  else if (choice == 1)
    {
      u = 1019u;
      g_variant_builder_add (&builder, "y", 0x33);
      g_variant_builder_add (&builder, "y", 0x44);
      g_variant_builder_add (&builder, "y", 0x95);
      g_variant_builder_add (&builder, "y", 0x2a);
      x1 = G_GINT64_CONSTANT (-333);
      x2 = G_GINT64_CONSTANT (1);
      mv = g_variant_new_string ("My spoon is too big.");
    }
  else if (choice == 2)
    {
      u = 5u;
      g_variant_builder_add (&builder, "y", 0x33);
      g_variant_builder_add (&builder, "y", 0x44);
      g_variant_builder_add (&builder, "y", 0x95);
      g_variant_builder_add (&builder, "y", 0x2a);
      g_variant_builder_add (&builder, "y", 0xb4);
      g_variant_builder_add (&builder, "y", 0x9c);
      g_variant_builder_add (&builder, "y", 0x2d);
      g_variant_builder_add (&builder, "y", 0x14);
      g_variant_builder_add (&builder, "y", 0x45);
      g_variant_builder_add (&builder, "y", 0xaa);
      x1 = G_GINT64_CONSTANT (5965);
      x2 = G_GINT64_CONSTANT (-3984);
      mv = g_variant_new_string ("!^@#@#^#$");
    }
  else
    {
      g_error ("Tried to use a choice for make_aggregate_event that hasn't "
               "been programmed.");
    }

  return g_variant_new (AGGREGATE_TYPE, u, &builder, x1, x2, mv);
}

static GVariant *
make_sequence_event (gint choice)
{
  GVariantBuilder builder_of_ay;
  g_variant_builder_init (&builder_of_ay, G_VARIANT_TYPE ("ay"));
  GVariantBuilder builder_of_axmv;
  g_variant_builder_init (&builder_of_axmv, G_VARIANT_TYPE ("a(xmv)"));
  guint32 u;

  if (choice == 0)
    {
      u = 1277u;
      g_variant_builder_add (&builder_of_ay, "y", 0x13);
      g_variant_builder_add (&builder_of_ay, "y", 0x37);
      gint64 x1 = G_GINT64_CONSTANT (1876);
      GVariant *mv1 = g_variant_new_double (3.14159);
      gint64 x2 = G_GINT64_CONSTANT (0);
      GVariant *mv2 = g_variant_new_string ("negative-1-point-steve");
      gint64 x3 = G_GINT64_CONSTANT (-1);
      GVariant *mv3 = NULL;
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x1, mv1);
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x2, mv2);
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x3, mv3);
    }
  else if (choice == 1)
    {
      u = 91912u;
      g_variant_builder_add (&builder_of_ay, "y", 0x13);
      g_variant_builder_add (&builder_of_ay, "y", 0x37);
      g_variant_builder_add (&builder_of_ay, "y", 0xd0);
      g_variant_builder_add (&builder_of_ay, "y", 0x0d);
      gint64 x1 = G_GINT64_CONSTANT (7);
      GVariant *mv1 = g_variant_new_double (2.71828); // Guess this number!
      gint64 x2 = G_GINT64_CONSTANT (67352);
      GVariant *mv2 = g_variant_new_string ("Help! I'm trapped in a testing string!");
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x1, mv1);
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x2, mv2);
    }
  else if (choice == 2)
    {
      u = 113u;
      g_variant_builder_add (&builder_of_ay, "y", 0xe1);
      gint64 x1 = G_GINT64_CONSTANT (747);
      GVariant *mv1 = NULL;
      gint64 x2 = G_GINT64_CONSTANT (57721);
      GVariant *mv2 = g_variant_new_string ("Secret message to the Russians: "
                                            "The 'rooster' has 'laid' an "
                                            "'egg'.");
      gint64 x3 = G_GINT64_CONSTANT (-100);
      GVariant *mv3 = g_variant_new_double (120.20569);
      gint64 x4 = G_GINT64_CONSTANT (127384);
      GVariant *mv4 = g_variant_new_double (-2.685452);

      g_variant_builder_add (&builder_of_axmv, "(xmv)", x1, mv1);
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x2, mv2);
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x3, mv3);
      g_variant_builder_add (&builder_of_axmv, "(xmv)", x4, mv4);
    }
  else
    {
      g_error ("Tried to use a choice for make_sequence_event that hasn't "
               "been programmed.");
    }

  return g_variant_new (SEQUENCE_TYPE, u, &builder_of_ay, &builder_of_axmv);
}

/*
 * Stores a single individual event and asserts that it (and nothing else) was
 * stored.
 */
static gboolean
store_single_individual_event (EmerPersistentCache *cache,
                               capacity_t          *capacity)
{
  GVariant *var = make_individual_event (0);
  GVariant *var_array[] = {var, NULL};
  GVariant *empty_array[] = {NULL};

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  gboolean success = emer_persistent_cache_store_metrics (cache,
                                                          var_array,
                                                          empty_array,
                                                          empty_array,
                                                          &num_individual_events_stored,
                                                          &num_aggregate_events_stored,
                                                          &num_sequence_events_stored,
                                                          capacity);
  g_variant_unref (var);

  g_assert (num_individual_events_stored == 1);
  g_assert (num_aggregate_events_stored == 0);
  g_assert (num_sequence_events_stored == 0);

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
  GVariant *var = make_aggregate_event (0);
  GVariant *var_array[] = {var, NULL};
  GVariant *empty_array[] = {NULL};

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  gboolean success = emer_persistent_cache_store_metrics (cache,
                                                          empty_array,
                                                          var_array,
                                                          empty_array,
                                                          &num_individual_events_stored,
                                                          &num_aggregate_events_stored,
                                                          &num_sequence_events_stored,
                                                          capacity);
  g_variant_unref (var);

  g_assert (num_individual_events_stored == 0);
  g_assert (num_aggregate_events_stored == 1);
  g_assert (num_sequence_events_stored == 0);

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
  GVariant *var = make_sequence_event (0);
  GVariant *var_array[] = {var, NULL};
  GVariant *empty_array[] = {NULL};

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  gboolean success = emer_persistent_cache_store_metrics (cache,
                                                          empty_array,
                                                          empty_array,
                                                          var_array,
                                                          &num_individual_events_stored,
                                                          &num_aggregate_events_stored,
                                                          &num_sequence_events_stored,
                                                          capacity);
  g_variant_unref (var);

  g_assert (num_individual_events_stored == 0);
  g_assert (num_aggregate_events_stored == 0);
  g_assert (num_sequence_events_stored == 1);

  return success;
}

static void
make_many_events (GVariant ***inds,
                  GVariant ***aggs,
                  GVariant ***seqs)
{
  *inds = g_new (GVariant *, 5 * sizeof (GVariant *));
  for (gint i = 0; i < 4; i++)
    (*inds)[i] = make_individual_event (i);
  (*inds)[4] = NULL;

  *aggs = g_new (GVariant *, 4 * sizeof (GVariant *));
  for (gint i = 0; i < 3; i++)
    (*aggs)[i] = make_aggregate_event (i);
  (*aggs)[3] = NULL;

  *seqs = g_new (GVariant *, 4 * sizeof (GVariant *));
  for (gint i = 0; i < 3; i++)
    (*seqs)[i] = make_sequence_event (i);
  (*seqs)[3] = NULL;
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
            gint                *num_individual_events_made,
            gint                *num_aggregate_events_made,
            gint                *num_sequence_events_made,
            gint                *num_individual_events_stored,
            gint                *num_aggregate_events_stored,
            gint                *num_sequence_events_stored,
            capacity_t          *capacity)
{
  GVariant **var_ind_array;
  GVariant **var_agg_array;
  GVariant **var_seq_array;

  make_many_events (&var_ind_array, &var_agg_array, &var_seq_array);

  *num_individual_events_made = c_array_len (var_ind_array);
  *num_aggregate_events_made = c_array_len (var_agg_array);
  *num_sequence_events_made = c_array_len (var_seq_array);

  gboolean success = emer_persistent_cache_store_metrics (cache,
                                                          var_ind_array,
                                                          var_agg_array,
                                                          var_seq_array,
                                                          num_individual_events_stored,
                                                          num_aggregate_events_stored,
                                                          num_sequence_events_stored,
                                                          capacity);

  free_variant_c_array (var_ind_array);
  free_variant_c_array (var_agg_array);
  free_variant_c_array (var_seq_array);

  return success;
}

static gboolean
check_all_gvariants_equal (GVariant **this_array,
                           GVariant **other_array)
{
  g_assert (this_array != NULL);
  g_assert (other_array != NULL);

  for (gint i = 0; this_array[i] != NULL || other_array[i] != NULL; i++)
    {
      if (this_array[i] == NULL || other_array[i] == NULL)
        g_error ("Array lengths are not equal.");

      if (!g_variant_equal (this_array[i], other_array[i]))
        {
          gchar *this_one = g_variant_print (this_array[i], TRUE);
          gchar *other_one = g_variant_print (other_array[i], TRUE);
          g_error ("%s is not equal to %s.", this_one, other_one);
        }
    }
  return TRUE;
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
test_persistent_cache_store_one_individual_event_succeeds (gboolean     *unused,
                                                           gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  gboolean success = store_single_individual_event (cache, &capacity);
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
  GVariant *ind = make_individual_event (0);
  GVariant *agg = make_aggregate_event (0);
  GVariant *seq = make_sequence_event (0);

  GVariant *ind_a[] = {ind, NULL};
  GVariant *agg_a[] = {agg, NULL};
  GVariant *seq_a[] = {seq, NULL};

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  capacity_t capacity;

  gboolean success = emer_persistent_cache_store_metrics (cache,
                                                          ind_a,
                                                          agg_a,
                                                          seq_a,
                                                          &num_individual_events_stored,
                                                          &num_aggregate_events_stored,
                                                          &num_sequence_events_stored,
                                                          &capacity);
  g_object_unref (cache);

  g_variant_unref (ind);
  g_variant_unref (agg);
  g_variant_unref (seq);

  g_assert (success);

  g_assert (num_individual_events_stored == 1);
  g_assert (num_aggregate_events_stored == 1);
  g_assert (num_sequence_events_stored == 1);

  g_assert (capacity == CAPACITY_LOW);
}

static void
test_persistent_cache_store_many_succeeds (gboolean     *unused,
                                           gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  gint num_individual_events_made;
  gint num_aggregate_events_made;
  gint num_sequence_events_made;
  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;
  capacity_t capacity;
  gboolean success =
    store_many (cache, &num_individual_events_made, &num_aggregate_events_made,
                &num_sequence_events_made, &num_individual_events_stored,
                &num_aggregate_events_stored, &num_sequence_events_stored,
                &capacity);
  g_object_unref (cache);
  g_assert (success);

  g_assert (num_individual_events_stored == num_individual_events_made);
  g_assert (num_aggregate_events_stored == num_aggregate_events_made);
  g_assert (num_sequence_events_stored == num_sequence_events_made);
}

static void
test_persistent_cache_store_when_full_succeeds (gboolean     *unused,
                                                gconstpointer dontuseme)
{
  gint space_in_bytes = 3000;
  EmerBootIdProvider *boot_id_provider =
    emer_boot_id_provider_new_full (TEST_DIRECTORY TEST_SYSTEM_BOOT_ID_FILE);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          space_in_bytes,
                                                          boot_id_provider);
  capacity_t capacity = CAPACITY_LOW;

  // Store a ton, until it is full.
  // TODO: Find a less hacky way of doing this.
  gint iterations = space_in_bytes / 150;
  for (gint i = 0; i < iterations; i++)
    {
      gint num_individual_events_made;
      gint num_aggregate_events_made;
      gint num_sequence_events_made;
      gint num_individual_events_stored;
      gint num_aggregate_events_stored;
      gint num_sequence_events_stored;
      gboolean success =
        store_many (cache, &num_individual_events_made,
                    &num_aggregate_events_made, &num_sequence_events_made,
                    &num_individual_events_stored, &num_aggregate_events_stored,
                    &num_sequence_events_stored, &capacity);
      g_assert (success);

      if (capacity == CAPACITY_MAX)
        {
          g_assert (num_individual_events_stored <= num_individual_events_made);
          g_assert (num_aggregate_events_stored <= num_aggregate_events_made);
          g_assert (num_sequence_events_stored <= num_sequence_events_made);
          break;
        }

      g_assert (num_individual_events_stored == num_individual_events_made);
      g_assert (num_aggregate_events_stored == num_aggregate_events_made);
      g_assert (num_sequence_events_stored == num_sequence_events_made);
    }

  g_object_unref (cache);
  g_object_unref (boot_id_provider);
  g_assert (capacity == CAPACITY_MAX);
}

static void
test_persistent_cache_drain_one_individual_succeeds (gboolean     *unused,
                                                     gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  GVariant *var = make_individual_event (1);
  GVariant *var_array[] = {var, NULL};
  GVariant *empty_array[] = {NULL};

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       var_array,
                                       empty_array,
                                       empty_array,
                                       &num_individual_events_stored,
                                       &num_aggregate_events_stored,
                                       &num_sequence_events_stored,
                                       &capacity);

  GVariant **ind_array;
  GVariant **agg_array;
  GVariant **seq_array;

  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &ind_array,
                                                          &agg_array,
                                                          &seq_array,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert (success);
  g_assert (check_all_gvariants_equal (var_array, ind_array));
  g_assert (check_all_gvariants_equal (empty_array, agg_array));
  g_assert (check_all_gvariants_equal (empty_array, seq_array));
  g_variant_unref (var);

  free_variant_c_array (ind_array);
  free_variant_c_array (agg_array);
  free_variant_c_array (seq_array);
}

static void
test_persistent_cache_drain_one_aggregate_succeeds (gboolean     *unused,
                                                    gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  GVariant *var = make_aggregate_event (1);
  GVariant *var_array[] = {var, NULL};
  GVariant *empty_array[] = {NULL};

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       empty_array,
                                       var_array,
                                       empty_array,
                                       &num_individual_events_stored,
                                       &num_aggregate_events_stored,
                                       &num_sequence_events_stored,
                                       &capacity);

  GVariant **ind_array;
  GVariant **agg_array;
  GVariant **seq_array;

  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &ind_array,
                                                          &agg_array,
                                                          &seq_array,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert (success);
  g_assert (check_all_gvariants_equal (empty_array, ind_array));
  g_assert (check_all_gvariants_equal (var_array, agg_array));
  g_assert (check_all_gvariants_equal (empty_array, seq_array));
  g_variant_unref (var);

  free_variant_c_array (ind_array);
  free_variant_c_array (agg_array);
  free_variant_c_array (seq_array);
}

static void
test_persistent_cache_drain_one_sequence_succeeds (gboolean     *unused,
                                                   gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  GVariant *var = make_sequence_event (1);
  GVariant *var_array[] = {var, NULL};
  GVariant *empty_array[] = {NULL};

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       empty_array,
                                       empty_array,
                                       var_array,
                                       &num_individual_events_stored,
                                       &num_aggregate_events_stored,
                                       &num_sequence_events_stored,
                                       &capacity);

  GVariant **ind_array;
  GVariant **agg_array;
  GVariant **seq_array;

  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &ind_array,
                                                          &agg_array,
                                                          &seq_array,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert (success);
  g_assert (check_all_gvariants_equal (empty_array, ind_array));
  g_assert (check_all_gvariants_equal (empty_array, agg_array));
  g_assert (check_all_gvariants_equal (var_array, seq_array));
  g_variant_unref (var);

  free_variant_c_array (ind_array);
  free_variant_c_array (agg_array);
  free_variant_c_array (seq_array);
}

static void
test_persistent_cache_drain_many_succeeds (gboolean     *unused,
                                           gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  // Fill it up first.
  GVariant **ind_array;
  GVariant **agg_array;
  GVariant **seq_array;
  make_many_events (&ind_array, &agg_array, &seq_array);

  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;

  capacity_t capacity;

  emer_persistent_cache_store_metrics (cache,
                                       ind_array,
                                       agg_array,
                                       seq_array,
                                       &num_individual_events_stored,
                                       &num_aggregate_events_stored,
                                       &num_sequence_events_stored,
                                       &capacity);

  // Check if we get the same things back.
  GVariant **new_ind_array;
  GVariant **new_agg_array;
  GVariant **new_seq_array;

  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &new_ind_array,
                                                          &new_agg_array,
                                                          &new_seq_array,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert (success);
  g_assert (check_all_gvariants_equal (ind_array, new_ind_array));
  g_assert (check_all_gvariants_equal (agg_array, new_agg_array));
  g_assert (check_all_gvariants_equal (seq_array, new_seq_array));

  free_variant_c_array (ind_array);
  free_variant_c_array (agg_array);
  free_variant_c_array (seq_array);

  free_variant_c_array (new_ind_array);
  free_variant_c_array (new_agg_array);
  free_variant_c_array (new_seq_array);
}

static void
test_persistent_cache_drain_empty_succeeds (gboolean     *unused,
                                            gconstpointer dontuseme)
{
  // Don't store anything.
  EmerPersistentCache *cache = make_testing_cache ();
  GVariant **ind_array;
  GVariant **agg_array;
  GVariant **seq_array;
  gboolean success = emer_persistent_cache_drain_metrics (cache,
                                                          &ind_array,
                                                          &agg_array,
                                                          &seq_array,
                                                          MAX_BYTES_TO_READ);
  g_object_unref (cache);

  g_assert (success);

  // Should contain logically empty arrays.
  GVariant *empty_array[] = {NULL};
  g_assert (check_all_gvariants_equal (ind_array, empty_array));
  g_assert (check_all_gvariants_equal (agg_array, empty_array));
  g_assert (check_all_gvariants_equal (seq_array, empty_array));
}

static void
test_persistent_cache_purges_when_out_of_date_succeeds (gboolean     *unused,
                                                        gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  gint num_individual_events_made;
  gint num_aggregate_events_made;
  gint num_sequence_events_made;
  gint num_individual_events_stored;
  gint num_aggregate_events_stored;
  gint num_sequence_events_stored;
  capacity_t capacity;
  store_many (cache, &num_individual_events_made, &num_aggregate_events_made,
              &num_sequence_events_made, &num_individual_events_stored,
              &num_aggregate_events_stored, &num_sequence_events_stored,
              &capacity);
  gboolean success =
    emer_persistent_cache_set_different_version_for_testing ();
  g_object_unref (cache);
  g_assert (success);

  EmerPersistentCache *cache2 = make_testing_cache ();
  // Metrics should all be purged now.
  GVariant **ind_array;
  GVariant **agg_array;
  GVariant **seq_array;

  emer_persistent_cache_drain_metrics (cache2,
                                       &ind_array,
                                       &agg_array,
                                       &seq_array,
                                       MAX_BYTES_TO_READ);
  g_object_unref (cache2);

  GVariant *empty_array[] = {NULL};
  g_assert (check_all_gvariants_equal (ind_array, empty_array));
  g_assert (check_all_gvariants_equal (agg_array, empty_array));
  g_assert (check_all_gvariants_equal (seq_array, empty_array));
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

  write_default_key_file_to_disk ();

  capacity_t capacity;

  // Insert a metric.
  store_single_individual_event (cache, &capacity);

  // Corrupt metafile.
  remove_offset ();

  // Reset cached metadata.
  g_object_unref (cache);
  EmerPersistentCache *cache2 = make_testing_cache ();

  // This call should detect corruption and wipe the cache of all previous
  // events. However, this new aggregate event should be stored.
  store_single_aggregate_event (cache2, &capacity);

  GVariant **ind_array;
  GVariant **agg_array;
  GVariant **seq_array;

  emer_persistent_cache_drain_metrics (cache2, &ind_array, &agg_array,
                                       &seq_array, MAX_BYTES_TO_READ);

  // Only an aggregate event should remain.
  g_assert (c_array_len (agg_array) == 1);
  g_assert (c_array_len (ind_array) == 0);

  g_object_unref (cache2);
}

/*
 * Test creates an default boot metafile. Then it corrupts the metafile by
 * removing the offset from it. Finally, a store call is made again to detect
 * the corruption and reset the metafile.
 */
static void
test_persistent_cache_resets_boot_metafile_when_boot_offset_corrupted (gboolean     *unused,
                                                                       gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();

  write_default_key_file_to_disk ();

  // Corrupt metafile.
  remove_offset ();

  // This call should detect corruption and reset the metafile.
  capacity_t capacity;
  store_single_aggregate_event (cache, &capacity);

  g_assert (read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache);
}

/*
 * Test triggers the computation of a new boot offset by storing metrics with no
 * preexisting boot metafile, which triggers a reset to offset 0 and the saved
 * boot id to the current boot id on the system. The persistent cache is
 * then unref'd and made anew. This causes the cached values to be lost. The
 * metafile must then be mutated to simulate a new boot. Then another storing of
 * metrics is needed to get the cache to compute a new offset.
 * Then we need to unref this and create it AGAIN to remove the cached values.
 * Finally, one more call to store should write new timestamps but shouldn't
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
  capacity_t capacity;
  store_single_individual_event (cache, &capacity);

  g_assert (read_whether_boot_offset_is_reset_value ());

  gint64 absolute_time, relative_time;
  g_assert (get_current_time (CLOCK_BOOTTIME, &relative_time) &&
            get_current_time (CLOCK_REALTIME, &absolute_time));

  g_object_unref (cache);
  set_boot_id_in_metafile (FAKE_BOOT_ID);

  EmerPersistentCache *cache2 = make_testing_cache ();

  // This call should have to compute the boot offset itself.
  store_single_aggregate_event (cache2, &capacity);

  g_assert (boot_timestamp_is_valid (relative_time, absolute_time));
  gint64 second_offset = read_offset ();

  // This should not have simply reset the metafile again.
  g_assert_false (read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache2);
  EmerPersistentCache *cache3 = make_testing_cache ();

  store_single_individual_event (cache3, &capacity);

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
test_persistent_cache_computes_reasonable_offset (gboolean *unused,
                                                  gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  store_single_individual_event (cache, &capacity);

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
  gint64 second_offset = read_offset();
  g_assert (second_offset <= first_offset + ACCEPTABLE_OFFSET_VARIANCE);
  g_assert (second_offset >= first_offset - ACCEPTABLE_OFFSET_VARIANCE);

  // This should not have simply reset the metafile again.
  g_assert (!read_whether_boot_offset_is_reset_value ());

  g_object_unref (cache2);
}

/*
 * Will test if the cached offset loading doesn't crash or produce unexpected
 * values by storing metrics in multiple *_store_metrics() calls without
 * cleaning up the metafile in between.
 *
 * This test does no special mutation of the metafile in the test case beyond
 * what the production code would normally do. Thus the offset will always be
 * reset to, and then cached to, 0.
 */
static void
test_persistent_cache_rebuilds_boot_metafile (gboolean     *unused,
                                              gconstpointer dontuseme)
{
  EmerPersistentCache *cache = make_testing_cache ();
  capacity_t capacity;
  store_single_individual_event (cache, &capacity);

  gint64 first_offset = read_offset ();
  gint64 absolute_time, relative_time;
  g_assert (get_current_time (CLOCK_BOOTTIME, &relative_time) &&
            get_current_time (CLOCK_REALTIME, &absolute_time));

  store_single_sequence_event (cache, &capacity);

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
  write_default_key_file_to_disk ();

  capacity_t capacity;
  store_single_individual_event (cache, &capacity);

  gint64 first_offset;
  GError *error = NULL;
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &first_offset,
                                                        &error, FALSE));
  g_assert_no_error (error);

  gint64 absolute_time, relative_time;
  g_assert (get_current_time (CLOCK_BOOTTIME, &relative_time) &&
            get_current_time (CLOCK_REALTIME, &absolute_time));

  // This value should never be read because the persistent cache should read
  // from its cached value next call.
  set_boot_offset_in_metafile (FAKE_RELATIVE_OFFSET);

  // This call should read the offset from its cached value, not the new one
  // from disk.
  store_single_individual_event (cache, &capacity);

  g_assert (boot_timestamp_is_valid (relative_time, absolute_time));

  gint64 second_offset;
  g_assert (emer_persistent_cache_get_boot_time_offset (cache, &second_offset,
                                                        &error, FALSE));
  g_assert_no_error (error);
  g_assert (first_offset == second_offset);

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
  ADD_CACHE_TEST_FUNC ("/persistent-cache/store-one-individual-event-succeeds",
                       test_persistent_cache_store_one_individual_event_succeeds);
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
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-one-individual-succeeds",
                       test_persistent_cache_drain_one_individual_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-one-aggregate-succeeds",
                       test_persistent_cache_drain_one_aggregate_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-one-sequence-succeeds",
                       test_persistent_cache_drain_one_sequence_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-many-succeeds",
                       test_persistent_cache_drain_many_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/drain-empty-succeeds",
                       test_persistent_cache_drain_empty_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/purges-when-out-of-date-succeeds",
                       test_persistent_cache_purges_when_out_of_date_succeeds);
  ADD_CACHE_TEST_FUNC ("/persistent-cache/rebuilds-boot-metafile",
                       test_persistent_cache_rebuilds_boot_metafile);
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
#undef ADD_CACHE_TEST_FUNC

  return g_test_run ();
}
