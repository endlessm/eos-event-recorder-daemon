/* Copyright 2014 Endless Mobile, Inc. */

#include "daemon/emer-persistent-cache.h"

#include <glib.h>
#include <stdio.h>
#include <glib/gstdio.h>

static gchar *TEST_DIRECTORY = "/tmp/metrics_testing/";

#define TEST_SIZE 1024000

// TODO: Replace this with a reasonable value once it is used.
#define MAX_BYTES_TO_READ 0

// ---- Helper functions come first ----

static void
tear_down_file (gchar *dir,
                gchar *file)
{
  gchar *combo = g_strconcat (dir, file, NULL);
  g_unlink (combo);
  g_free (combo);
  return;
}

static void
tear_down_files (gchar *directory)
{
  tear_down_file (directory, CACHE_PREFIX INDIVIDUAL_SUFFIX);
  tear_down_file (directory, CACHE_PREFIX AGGREGATE_SUFFIX);
  tear_down_file (directory, CACHE_PREFIX SEQUENCE_SUFFIX);
  tear_down_file (directory, CACHE_PREFIX LOCAL_CACHE_VERSION_METAFILE);
  g_rmdir (directory);
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
test_persistent_cache_new_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  GError *error = NULL;
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          &error,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
  g_assert (cache != NULL);
  g_object_unref (cache);
  if (error != NULL)
    g_error_free (error);
  g_assert (error == NULL);
}

static void
test_persistent_cache_store_one_individual_event_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
  capacity_t capacity;
  gboolean success = store_single_individual_event (cache, &capacity);
  g_object_unref (cache);
  g_assert (success);
  g_assert (capacity == CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_aggregate_event_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
  capacity_t capacity;
  gboolean success = store_single_aggregate_event (cache, &capacity);
  g_object_unref (cache);
  g_assert (success);
  g_assert (capacity == CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_sequence_event_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
  capacity_t capacity;
  gboolean success = store_single_sequence_event (cache, &capacity);
  g_object_unref (cache);
  g_assert (success);
  g_assert (capacity == CAPACITY_LOW);
}

static void
test_persistent_cache_store_one_of_each_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
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
test_persistent_cache_store_many_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);

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
test_persistent_cache_store_when_full_succeeds (void)
{
  gint space_in_bytes = 3000;
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          space_in_bytes);
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
  g_assert (capacity == CAPACITY_MAX);
}

static void
test_persistent_cache_drain_one_individual_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
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
test_persistent_cache_drain_one_aggregate_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
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
test_persistent_cache_drain_one_sequence_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
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
test_persistent_cache_drain_many_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);

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
test_persistent_cache_drain_empty_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  // Don't store anything.
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
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
test_persistent_cache_purges_when_out_of_date_succeeds (void)
{
  tear_down_files (TEST_DIRECTORY);
  EmerPersistentCache *cache = emer_persistent_cache_new (NULL,
                                                          NULL,
                                                          TEST_DIRECTORY,
                                                          TEST_SIZE);
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

  EmerPersistentCache *cache2 = emer_persistent_cache_new (NULL,
                                                           NULL,
                                                           TEST_DIRECTORY,
                                                           TEST_SIZE);
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

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

  g_test_add_func ("/persistent-cache/new-succeeds",
                   test_persistent_cache_new_succeeds);
  g_test_add_func ("/persistent-cache/store-one-individual-event-succeeds",
                   test_persistent_cache_store_one_individual_event_succeeds);
  g_test_add_func ("/persistent-cache/store-one-aggregate-event-succeeds",
                   test_persistent_cache_store_one_aggregate_event_succeeds);
  g_test_add_func ("/persistent-cache/store-one-sequence-event-succeeds",
                   test_persistent_cache_store_one_sequence_event_succeeds);
  g_test_add_func ("/persistent-cache/store-one-of-each-succeeds",
                   test_persistent_cache_store_one_of_each_succeeds);
  g_test_add_func ("/persistent-cache/store-many-succeeds",
                   test_persistent_cache_store_many_succeeds);
  g_test_add_func ("/persistent-cache/store-when-full-succeeds",
                   test_persistent_cache_store_when_full_succeeds);
  g_test_add_func ("/persistent-cache/drain-one-individual-succeeds",
                   test_persistent_cache_drain_one_individual_succeeds);
  g_test_add_func ("/persistent-cache/drain-one-aggregate-succeeds",
                   test_persistent_cache_drain_one_aggregate_succeeds);
  g_test_add_func ("/persistent-cache/drain-one-sequence-succeeds",
                   test_persistent_cache_drain_one_sequence_succeeds);
  g_test_add_func ("/persistent-cache/drain-many-succeeds",
                   test_persistent_cache_drain_many_succeeds);
  g_test_add_func ("/persistent-cache/drain-empty-succeeds",
                   test_persistent_cache_drain_empty_succeeds);
  g_test_add_func ("/persistent-cache/purges-when-out-of-date-succeeds",
                   test_persistent_cache_purges_when_out_of_date_succeeds);

  return g_test_run ();
}
