/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-persistent-cache.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * SECTION:emer-persistent-cache.c
 * @title: Persistent Cache
 * @short_description: Stores metrics locally (on the user's machine).
 * @include: eosmetrics/eosmetrics.h
 *
 * The Persistent Cache is the sink to which an event recorder flushes
 * metrics. It will store these metrics until a drain operation is
 * requested or the Persistent Cache is purged due to versioning.
 *
 * Should the cached metrics occupy more than MAX_CACHE_SIZE bytes, the
 * Persistent Cache will begin ignoring new metrics until the old ones have
 * been removed.
 *
 * If the CURRENT_CACHE_VERSION is incremented to indicate
 * backwards-incompatible versioning, any cached metrics will be deleted, and
 * the file indicating the local cache version will be updated to the reflect
 * the new version number.
 */

static gboolean   append_metric                       (EmerPersistentCache *self,
                                                       GFile               *file,
                                                       GVariant            *metric);

static gboolean   apply_cache_versioning              (EmerPersistentCache *self,
                                                       GCancellable        *cancellable,
                                                       GError             **error);

static gboolean   cache_has_room                      (EmerPersistentCache *self,
                                                       gsize                size);

static gboolean   drain_metrics_file                  (EmerPersistentCache *self,
                                                       GVariant          ***return_list,
                                                       gchar               *path_ending,
                                                       gchar               *variant_type);

static GVariant*  flip_bytes_if_big_endian_machine    (GVariant            *variant);

static void       free_variant_list                   (GVariant           **list);

static GFile*     get_cache_file                      (gchar               *path_ending);

static void       emer_persistent_cache_initable_init (GInitableIface      *iface);

static gboolean   load_cache_size                     (EmerPersistentCache *self,
                                                       GCancellable        *cancellable,
                                                       GError             **error);

static gboolean   load_local_cache_version            (gint64              *version);

static gboolean   emer_persistent_cache_may_fail_init (GInitable           *self,
                                                       GCancellable        *cancellable,
                                                       GError             **error);

static gboolean   purge_cache_files                   (EmerPersistentCache *self,
                                                       GCancellable        *cancellable,
                                                       GError             **error);

static gboolean   store_metric_list                   (EmerPersistentCache *self,
                                                       GFile               *file,
                                                       GVariant           **list,
                                                       gint                *num_variants_stored,
                                                       capacity_t          *capacity);

static gboolean   update_cache_version_number         (GCancellable        *cancellable,
                                                       GError             **error);

static capacity_t update_capacity                     (EmerPersistentCache *self);

typedef struct GVariantWritable
{
  gsize length;
  gpointer data;
} GVariantWritable;

typedef struct _EmerPersistentCachePrivate
{
  guint64 cache_size;
  capacity_t capacity;
} EmerPersistentCachePrivate;

G_DEFINE_TYPE_WITH_CODE (EmerPersistentCache, emer_persistent_cache, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (EmerPersistentCache)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, emer_persistent_cache_initable_init))

/*
 * If this version is greater than the version of the persisted metrics,
 * they will be purged from the system, and the file indicating which
 * version the persisted metrics currently have will be updated.
 */
#define CURRENT_CACHE_VERSION 2

/*
 * The point at which the capacity switches fron LOW to HIGH.
 */
#define HIGH_CAPACITY_THRESHOLD 0.75 // 75%

#define PERSISTENT_CACHE_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), EMER_TYPE_PERSISTENT_CACHE, EmerPersistentCachePrivate))

/*
 * The largest amount of memory (in bytes) that the metrics cache may
 * occupy before incoming metrics are ignored.
 *
 * Should never be altered in production code!
 */
static gint MAX_CACHE_SIZE = 102400; // 100 kB

/*
 * The directory metrics and their meta-file are saved to.
 * Is listed in all caps because it should be treated as though
 * it were immutable by production code.  Only testing code should
 * ever alter this variable.
 */
static gchar* CACHE_DIRECTORY = "/var/cache/metrics/";

static void
emer_persistent_cache_class_init (EmerPersistentCacheClass *klass)
{

}

static void
emer_persistent_cache_init (EmerPersistentCache *self)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  priv->cache_size = 0L;
  priv->capacity = CAPACITY_LOW;
}

static gboolean
emer_persistent_cache_may_fail_init (GInitable    *self,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  gboolean versioning_success = apply_cache_versioning (EMER_PERSISTENT_CACHE (self),
                                                        cancellable,
                                                        error);
  if (!versioning_success)
    return FALSE;

  return load_cache_size (EMER_PERSISTENT_CACHE (self), cancellable, error);
}

static void
emer_persistent_cache_initable_init (GInitableIface *iface)
{
  iface->init = emer_persistent_cache_may_fail_init;
}

/*
 * Constructor for creating a new Persistent Cache.
 * Returns a singleton instance of a Persistent Cache.
 * Please use this in production instead of the testing constructor!
 */
EmerPersistentCache*
emer_persistent_cache_get_default (GCancellable *cancellable,
                                   GError      **error)
{
  static EmerPersistentCache *singleton_cache;
  G_LOCK_DEFINE_STATIC (singleton_cache);

  G_LOCK (singleton_cache);
  if (singleton_cache == NULL)
    singleton_cache = g_initable_new (EMER_TYPE_PERSISTENT_CACHE,
                                      cancellable,
                                      error,
                                      NULL);
  G_UNLOCK (singleton_cache);

  return singleton_cache;
}

/*
 * You should use emer_persistent_cache_get_default() instead of this function.
 * Function should only be used in testing code, NOT in production code.
 * Should always use a custom directory.  A custom cache size may be specified,
 * but if set to 0, the production value will be used.
 */
EmerPersistentCache*
emer_persistent_cache_new (GCancellable *cancellable,
                           GError      **error,
                           gchar        *custom_directory,
                           gint          custom_cache_size)
{
  MAX_CACHE_SIZE = custom_cache_size;
  CACHE_DIRECTORY = custom_directory;
  return g_initable_new (EMER_TYPE_PERSISTENT_CACHE,
                         cancellable,
                         error,
                         NULL);
}

/*
 * Will transfer all metrics in the corresponding file into the out parameter
 * 'return_list'. The list will be NULL-terminated.  Returns TRUE on success,
 * and FALSE if any I/O error occured. Contents of return_list are undefined if
 * the return value is FALSE.
 */
static gboolean
drain_metrics_file (EmerPersistentCache *self,
                    GVariant          ***return_list,
                    gchar               *path_ending,
                    gchar               *variant_type)
{
  GError *error = NULL;
  GFile *file = get_cache_file (path_ending);
  GFileInputStream *file_stream = g_file_read (file, NULL, &error);

  if (file_stream == NULL)
    {
      g_critical ("Failed to open input stream to drain metrics. Error: %s.",
                   error->message);
      g_error_free (error);
      g_object_unref (file);
      return FALSE;
    }
  GInputStream *stream = G_INPUT_STREAM (file_stream);

  GArray *dynamic_array = g_array_new (FALSE, FALSE, sizeof (GVariant *));

  while (TRUE)
    {
      GVariantWritable writable;
      gssize length_bytes_read = g_input_stream_read (stream,
                                                      &writable.length,
                                                      sizeof (gsize),
                                                      NULL,
                                                      &error);
      if (length_bytes_read == 0) // EOF
        break;

      if (error != NULL)
        {
          gchar *fpath = g_file_get_path (file);
          g_critical ("Failed to read length of metric from input stream to "
                      "drain metrics. File: %s . Error: %s.", fpath,
                      error->message);
          g_free (fpath);
          g_error_free (error);
          g_object_unref (stream);
          g_object_unref (file);
          g_array_free (dynamic_array, TRUE);
          return FALSE;
        }
      if (length_bytes_read != sizeof (gsize))
        {
          g_critical ("We read %i bytes but expected %u bytes!",
                      length_bytes_read, sizeof (gsize));
          g_object_unref (stream);
          g_object_unref (file);
          g_array_free (dynamic_array, TRUE);
          return FALSE;
        }

      writable.data = g_new (guchar, writable.length);
      gssize data_bytes_read = g_input_stream_read (stream,
                                                    writable.data,
                                                    writable.length,
                                                    NULL,
                                                    &error);
      if (data_bytes_read != writable.length)
        {
          g_critical ("We read %i bytes of metric data when looking for %u!",
                      data_bytes_read, writable.length);
          return FALSE;
        }
      if (error != NULL)
        {
          gchar *fpath = g_file_get_path (file);
          g_critical ("Failed to read metric from input stream to drain metrics."
                      " File: %s . Error: %s.", fpath, error->message);
          g_free (fpath);
          g_error_free (error);
          g_object_unref (stream);
          g_object_unref (file);
          g_array_free (dynamic_array, TRUE);
          return FALSE;
        }

      // Deserialize
      GVariant *current_metric =
        g_variant_new_from_data (G_VARIANT_TYPE (variant_type),
                                 writable.data,
                                 writable.length,
                                 FALSE,
                                 (GDestroyNotify) g_free,
                                 writable.data);

      // Correct byte_ordering if necessary.
      GVariant *native_endian_metric =
        flip_bytes_if_big_endian_machine (current_metric);

      g_variant_unref (current_metric);
      g_array_append_val (dynamic_array, native_endian_metric);
    }
  g_object_unref (stream);
  g_object_unref (file);

  *return_list = g_new (GVariant *, dynamic_array->len + 1);
  memcpy (*return_list, dynamic_array->data,
          dynamic_array->len * sizeof (GVariant *));
  (*return_list)[dynamic_array->len] = NULL;
  g_array_free (dynamic_array, TRUE);

  return TRUE;
}

/*
 * Will transfer all metrics on disk into the three out parameters.
 * Each list is NULL-terminated. Will delete all metrics from disk immediately
 * afterward. Returns %TRUE on success. If any I/O operation fails, will return
 * %FALSE and the out parameters' contents will be undefined. max_num_bytes is
 * currently ignored. TODO: Actually use max_num_bytes to limit the amount of
 * metrics drained.
 */
gboolean
emer_persistent_cache_drain_metrics (EmerPersistentCache  *self,
                                     GVariant           ***list_of_individual_metrics,
                                     GVariant           ***list_of_aggregate_metrics,
                                     GVariant           ***list_of_sequence_metrics,
                                     gint                  max_num_bytes)
{
  gboolean ind_success = drain_metrics_file (self,
                                             list_of_individual_metrics,
                                             INDIVIDUAL_SUFFIX,
                                             INDIVIDUAL_TYPE);
  if (!ind_success)
    return FALSE;

  gboolean agg_success = drain_metrics_file (self,
                                             list_of_aggregate_metrics,
                                             AGGREGATE_SUFFIX,
                                             AGGREGATE_TYPE);
  if (!agg_success)
    {
      free_variant_list (*list_of_individual_metrics);
      return FALSE;
    }

  gboolean seq_success = drain_metrics_file (self,
                                             list_of_sequence_metrics,
                                             SEQUENCE_SUFFIX,
                                             SEQUENCE_TYPE);
  if (!seq_success)
    {
      free_variant_list (*list_of_individual_metrics);
      free_variant_list (*list_of_aggregate_metrics);
      return FALSE;
    }

  GError *error = NULL;
  if (!purge_cache_files (self, NULL, &error))
    {
      // The error has served its purpose in the previous call.
      g_error_free (error);
      return FALSE;
    }

  return TRUE;
}

/*
 * Frees all resources contained within a GVariant* list, including
 * the list itself.
 */
static void
free_variant_list (GVariant **list)
{
  g_return_if_fail (list != NULL);

  for (gint i = 0; list[i] != NULL; i++)
    g_variant_unref (list[i]);
  g_free (list);
}

/*
 * Attempts to write a NULL-terminated list of GVariant metrics to a given file.
 * Will update num_variants_stored and capacity given to it. Automatically
 * increments priv->cache_size. Returns %FALSE if any metrics failed due to
 * I/O error. %TRUE otherwise. Regardless of success or failure,
 * num_variants_stored and the capacity will be correctly set.
 */
static gboolean
store_metric_list (EmerPersistentCache *self,
                   GFile               *file,
                   GVariant           **list,
                   gint                *num_variants_stored,
                   capacity_t          *capacity)
{
  gboolean success = TRUE;
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  if (list == NULL)
    g_error ("Attempted to store a GVariant list that was actually "
             "a NULL pointer!");

  gint i;
  for (i = 0; list[i] != NULL; i++)
    {
      GVariant *current_metric = list[i];
      gsize metric_size = sizeof (gsize) + g_variant_get_size (current_metric);
      if (cache_has_room (self, metric_size))
        {
          success = append_metric (self, file, current_metric);
          if (!success)
            {
              g_critical ("Failed to write metric. Dropping some metrics.");
              break;
            }
          priv->cache_size += metric_size;
        }
      else
        {
          // This is how we know we are at max capacity --
          // we've been dropping metrics.
          priv->capacity = CAPACITY_MAX;
          break;
        }
    }

  *capacity = update_capacity (self);
  *num_variants_stored = i;
  return success;
}

/*
 * Will store all metrics passed to it onto disk if doing so would not exceed
 * its space limitation.  Each list must be NULL-terminated or NULL itself.
 * Will return the capacity of the cache via the out parameter 'capacity'.
 * Returns %TRUE on success, even if the metrics are intentionally dropped due
 * to space limitations.  Returns %FALSE only on I/O error.
 *
 * Regardless of success or failure, num_individual_metrics_stored,
 * num_aggregate_metrics_stored, num_sequence_metrics_stored, and capacity will
 * be correctly set.
 *
 * GVariants are assumed to be in the native endianness of the machine.
 */
gboolean
emer_persistent_cache_store_metrics (EmerPersistentCache  *self,
                                     GVariant            **list_of_individual_metrics,
                                     GVariant            **list_of_aggregate_metrics,
                                     GVariant            **list_of_sequence_metrics,
                                     gint                 *num_individual_metrics_stored,
                                     gint                 *num_aggregate_metrics_stored,
                                     gint                 *num_sequence_metrics_stored,
                                     capacity_t           *capacity)
{
  GFile *ind_file = get_cache_file (INDIVIDUAL_SUFFIX);
  gboolean ind_success = store_metric_list (self,
                                            ind_file,
                                            list_of_individual_metrics,
                                            num_individual_metrics_stored,
                                            capacity);
  g_object_unref (ind_file);
  if (!ind_success)
    {
      *num_aggregate_metrics_stored = 0;
      *num_sequence_metrics_stored = 0;
      return FALSE;
    }

  GFile *agg_file = get_cache_file (AGGREGATE_SUFFIX);
  gboolean agg_success = store_metric_list (self,
                                            agg_file,
                                            list_of_aggregate_metrics,
                                            num_aggregate_metrics_stored,
                                            capacity);
  g_object_unref (agg_file);
  if (!agg_success)
    {
      *num_sequence_metrics_stored = 0;
      return FALSE;
    }

  GFile *seq_file = get_cache_file (SEQUENCE_SUFFIX);
  gboolean seq_success = store_metric_list (self,
                                            seq_file,
                                            list_of_sequence_metrics,
                                            num_sequence_metrics_stored,
                                            capacity);
  g_object_unref (seq_file);
  if (!seq_success)
    return FALSE;

  /*
   * num_individual_metrics_stored, num_aggregate_metrics_stored,
   * num_sequence_metrics_stored, and capacity are updated within
   * store_metric_list.
   */
  return TRUE;
}

/*
 * Creates and returns a newly allocated GFile * corresponding to the cache
 * file ending or metafile ending given to it as path_ending.
 */
static GFile *
get_cache_file (gchar *path_ending)
{
  gchar *path;
  path = g_strconcat (CACHE_DIRECTORY, CACHE_PREFIX, path_ending, NULL);
  GFile *file = g_file_new_for_path (path);
  g_free (path);
  return file;
}

/*
 * Should only be called if the version is out of date, corrupted, or the cache
 * file does not exist. Replaces the content of all cache files with the empty
 * string. Will create these empty files if they do not already exist.
 * Returns TRUE on success and FALSE on I/O failure.
 */
static gboolean
purge_cache_files (EmerPersistentCache *self,
                   GCancellable        *cancellable,
                   GError             **error)
{
  GFile *ind_file = get_cache_file (INDIVIDUAL_SUFFIX);
  gboolean success =
    g_file_replace_contents (ind_file, "", 0, NULL, FALSE,
                             G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
                             NULL, cancellable, error);
  if (!success)
    {
      if (error != NULL)
        g_critical ("Failed to purge cache files. Error: %s.",
                    (*error)->message);
      else
        g_critical ("Failed to purge cache files.");
      g_object_unref (ind_file);
      return FALSE;
    }
  g_object_unref (ind_file);

  GFile *agg_file = get_cache_file (AGGREGATE_SUFFIX);
  success =
    g_file_replace_contents (agg_file, "", 0, NULL, FALSE,
                             G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
                             NULL, cancellable, error);
  if (!success)
    {
      if (error != NULL)
        g_critical ("Failed to purge cache files. Error: %s.",
                    (*error)->message);
      else
        g_critical ("Failed to purge cache files.");
      g_object_unref (agg_file);
      return FALSE;
    }
  g_object_unref (agg_file);

  GFile *seq_file = get_cache_file (SEQUENCE_SUFFIX);
  success =
    g_file_replace_contents (seq_file, "", 0, NULL, FALSE,
                             G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
                             NULL, cancellable, error);
  if (!success)
    {
      if (error != NULL)
        g_critical ("Failed to purge cache files. Error: %s.",
                    (*error)->message);
      else
        g_critical ("Failed to purge cache files.");
      g_object_unref (seq_file);
      return FALSE;
    }
  g_object_unref (seq_file);

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  priv->cache_size = 0L;
  priv->capacity = CAPACITY_LOW;

  return TRUE;
}

/*
 * Loads the local cache version into memory from disk.
 * Returns %TRUE on success and %FALSE on I/O error including
 * if the file doesn't exist.
 */
static gboolean
load_local_cache_version (gint64 *version)
{
  gchar *filepath = g_strconcat (CACHE_DIRECTORY,
                                 LOCAL_CACHE_VERSION_METAFILE,
                                 NULL);
  gchar *version_string = NULL;

  if (g_file_get_contents (filepath, &version_string, NULL, NULL))
    {
      gint64 version_int = g_ascii_strtoll (version_string, NULL, 10);
      if (version_int == 0) // Error code for failure.
        {
          gint e = errno;
          const gchar *err_str = g_strerror (e);
          g_critical ("Version file seems to be corrupted. Error: %s.", err_str);
          return FALSE;
        }
      *version = version_int;

      g_free (version_string);
      g_free (filepath);
      return TRUE;
    }
  else
    {
      g_free (filepath);
      return FALSE;
    }
}

/*
 * Attempts to append (cache) a metric at the end of a file.
 * Returns TRUE on success and FALSE on failure.
 */
static gboolean
append_metric (EmerPersistentCache *self,
               GFile               *file,
               GVariant            *metric)
{
  GError *error = NULL;
  GFileOutputStream *stream = g_file_append_to (file,
                                                G_FILE_CREATE_NONE,
                                                NULL,
                                                &error);
  if (stream == NULL)
    {
      gchar *path = g_file_get_path (file);
      g_critical ("Failed to open stream to cache file: %s . Error: %s.",
                  path, error->message);
      g_free (path);
      g_error_free (error);
      return FALSE;
    }

  GVariant *native_endian_metric = flip_bytes_if_big_endian_machine (metric);
  GVariantWritable writable;
  writable.length = g_variant_get_size (native_endian_metric);
  writable.data = (gpointer) g_variant_get_data (native_endian_metric);

  GString *writable_string = g_string_new ("");
  g_string_append_len (writable_string,
                       (const gchar *) &writable.length,
                       sizeof (writable.length));
  g_string_append_len (writable_string, writable.data, writable.length);
  gboolean success = g_output_stream_write_all (G_OUTPUT_STREAM (stream),
                                                writable_string->str,
                                                writable_string->len,
                                                NULL,
                                                NULL,
                                                &error);
  g_string_free (writable_string, TRUE);
  g_variant_unref (native_endian_metric);

  if (!success)
    {
      gchar *path = g_file_get_path (file);
      g_critical ("Failed to write to cache file: %s . Error: %s.",
                   path, error->message);
      g_object_unref (stream);
      g_free (path);
      g_error_free (error);
      return FALSE;
    }

  g_object_unref (stream);
  return TRUE;
}

/*
 * Will return a GVariant* with the bytes set to the opposite byte_order if
 * the machine is a big-endian machine.
 *
 * The returned GVariant* should have g_variant_unref() called on it when it is
 * no longer needed.
 */
static GVariant *
flip_bytes_if_big_endian_machine (GVariant *variant)
{
  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    return g_variant_byteswap (variant);
  else if (G_BYTE_ORDER != G_LITTLE_ENDIAN)
    g_error ("Holy crap! This machine is neither big NOR little-endian, "
             "time to panic. AAHAHAHAHAH!");
  return g_variant_ref (variant);
}

/*
 * Updates the metafile cache version number and creates a new meta_file if
 * one doesn't exist. Returns %TRUE on success, and %FALSE on failure.
 */
static gboolean
update_cache_version_number (GCancellable *cancellable,
                             GError       **error)
{
  GFile *meta_file = get_cache_file (LOCAL_CACHE_VERSION_METAFILE);
  gchar *version_string = g_strdup_printf ("%i", CURRENT_CACHE_VERSION);
  gsize version_size = strlen (version_string);
  gboolean success = g_file_replace_contents (meta_file,
                                              version_string,
                                              version_size,
                                              NULL,
                                              FALSE,
                                              G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
                                              NULL,
                                              cancellable,
                                              error);
  g_object_unref (meta_file);
  g_free (version_string);
  return success;
}

/*
 * Testing function that overwrites the metafile with an "older" version number.
 * Returns TRUE on success, FALSE on failure.
 */
gboolean
emer_persistent_cache_set_different_version_for_testing (void)
{
  gint diff_version = CURRENT_CACHE_VERSION - 1;
  gchar *ver_string = g_strdup_printf ("%i", diff_version);
  gsize ver_size = strlen (ver_string);
  GFile *meta_file = get_cache_file (LOCAL_CACHE_VERSION_METAFILE);
  gboolean success = g_file_replace_contents (meta_file, ver_string, ver_size,
                                              NULL, FALSE, G_FILE_CREATE_NONE,
                                              NULL, NULL, NULL);
  g_object_unref (meta_file);
  g_free (ver_string);
  return success;
}

/*
 * If the cache version file is out of date or not found, it wil attempt to
 * remove all cached metrics. If it succeeds, it will update the cache version
 * file to the new_version provided. Will create the cache directory if it
 * doesn't exist. Returns %TRUE on success and %FALSE on failure.
 */
static gboolean
apply_cache_versioning (EmerPersistentCache *self,
                        GCancellable        *cancellable,
                        GError             **error)
{
  if (g_mkdir_with_parents (CACHE_DIRECTORY, 0777) != 0)
    {
      const gchar *err_str = g_strerror (errno); // Don't free.
      g_critical ("Failed to create directory: %s . Error: %s.",
                  CACHE_DIRECTORY, err_str);
      return FALSE;
    }

  gint64 old_version;

  // We don't care about the error here.
  gboolean could_find = load_local_cache_version (&old_version);
  if (!could_find || CURRENT_CACHE_VERSION != old_version)
    {
      gboolean success = purge_cache_files (self, cancellable, error);
      if (!success)
        {
          if (error != NULL)
            g_critical ("Failed to purge cache files! Will not update version "
                        "number. Error: %s.", (*error)->message);
          else
            g_critical ("Failed to purge cache files! Will not update version "
                        "number.");
          return FALSE;
        }

      success = update_cache_version_number (cancellable, error);
      if (!success)
        {
          if (error != NULL)
            g_critical ("Failed to update cache version number to %i. "
                        "Error: %s.", CURRENT_CACHE_VERSION, (*error)->message);
          else
            g_critical ("Failed to update cache version number to %i.",
                        CURRENT_CACHE_VERSION);
          return FALSE;
        }
    }

  return TRUE;
}

/*
 * Sets the cache_size variable of self to the total size in bytes of the cache
 * files. Returns %TRUE on success.
 */
static gboolean
load_cache_size (EmerPersistentCache *self,
                 GCancellable        *cancellable,
                 GError             **error)
{
  GFile *dir = g_file_new_for_path (CACHE_DIRECTORY);
  guint64 disk_used;
  gboolean success = g_file_measure_disk_usage (dir,
                                                G_FILE_MEASURE_REPORT_ANY_ERROR,
                                                NULL,
                                                NULL,
                                                NULL,
                                                &disk_used,
                                                NULL,
                                                NULL,
                                                error);
  g_object_unref (dir);
  if (!success)
    {
      if (error != NULL)
        g_critical ("Failed to measure disk usage. Error: %s.",
                    (*error)->message);
      else
        g_critical ("Failed to measure disk usage.");
      return FALSE;
    }

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  priv->cache_size = disk_used;
  update_capacity (self);
  return success;
}

/*
 * Returns a hint (capacity_t) as to how filled up the cache is and
 * updates the internal value of capacity.
 */
static capacity_t
update_capacity (EmerPersistentCache *self)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  if (priv->capacity == CAPACITY_MAX)
    return CAPACITY_MAX;

  if (priv->cache_size >= HIGH_CAPACITY_THRESHOLD * MAX_CACHE_SIZE)
    priv->capacity = CAPACITY_HIGH;
  else
    priv->capacity = CAPACITY_LOW;

  return priv->capacity;
}

/*
 * Will check if the cache can store 'size' additional bytes. Returns
 * %TRUE if it can, %FALSE otherwise.
 */
static gboolean
cache_has_room (EmerPersistentCache *self,
                gsize                size)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  if (priv->capacity == CAPACITY_MAX)
    return FALSE;
  return priv->cache_size + size <= MAX_CACHE_SIZE;
}
