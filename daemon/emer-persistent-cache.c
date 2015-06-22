/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

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

#include <gio/gio.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>

#include <errno.h>

#include <eosmetrics/eosmetrics.h>

#include "shared/metrics-util.h"

/* SECTION:emer-persistent-cache.c
 * @title: Persistent Cache
 * @short_description: Stores metrics locally (on the user's machine).
 *
 * The persistent cache is the sink to which an event recorder flushes
 * metrics. It will store these metrics until a drain operation is
 * requested or the persistent cache is purged due to versioning.
 *
 * Should the cached metrics occupy more than the maximum allowed cache size,
 * the persistent cache will begin ignoring new metrics until the old ones have
 * been removed.
 *
 * If the CURRENT_CACHE_VERSION is incremented to indicate
 * backwards-incompatible versioning, any cached metrics will be deleted, and
 * the file indicating the local cache version will be updated to the reflect
 * the new version number.
 */

typedef struct _EmerPersistentCachePrivate
{
  EmerCacheSizeProvider *cache_size_provider;
  guint64 cache_size;
  capacity_t capacity;

  EmerBootIdProvider *boot_id_provider;

  EmerCacheVersionProvider *cache_version_provider;

  guint boot_offset_update_timeout_source_id;

  gchar *cache_directory;
  gchar *boot_metadata_file_path;

  gint64 boot_offset;
  gboolean boot_offset_initialized;

  uuid_t saved_boot_id;
  gboolean boot_id_initialized;

  GKeyFile *boot_offset_key_file;
} EmerPersistentCachePrivate;

static void emer_persistent_cache_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (EmerPersistentCache, emer_persistent_cache, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (EmerPersistentCache)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, emer_persistent_cache_initable_iface_init))

/*
 * If this version is greater than the version of the persisted metrics,
 * they will be purged from the system, and the file indicating which
 * version the persisted metrics currently have will be updated.
 */
#define CURRENT_CACHE_VERSION 3

/*
 * The point at which the capacity switches fron LOW to HIGH.
 */
#define HIGH_CAPACITY_THRESHOLD 0.75 // 75%

/*
 * The expected size in bytes of the file located at SYSTEM_BOOT_ID_FILE.
 * The file should be 37 lower-case hexadecimal characters or hyphens followed
 * by a newline character.
 *
 * This is also the length of the string representation of the boot id. It has
 * the same characters other than the newline character is instead a null
 * character.
 */
#define BOOT_ID_FILE_LENGTH 37

/*
 * The amount of time (in seconds) between every periodic update to the boot
 * offset file. This will primarily keep our relative timestamps more accurate.
 */
#define DEFAULT_BOOT_TIMESTAMPS_UPDATE (60u * 60u)

/*
 * The path to the file containing the boot ID used to determine if this is the
 * same boot as previous metrics were recorded in or not.
 */
#define SYSTEM_BOOT_ID_FILE "/proc/sys/kernel/random/boot_id"

G_LOCK_DEFINE_STATIC (update_boot_offset);

enum
{
  PROP_0,
  PROP_CACHE_DIRECTORY,
  PROP_CACHE_SIZE_PROVIDER,
  PROP_BOOT_ID_PROVIDER,
  PROP_CACHE_VERSION_PROVIDER,
  PROP_BOOT_OFFSET_UPDATE_INTERVAL,
  NPROPS
};

static GParamSpec *emer_persistent_cache_props[NPROPS] = { NULL, };

/*
 * Will read the boot id from a metadata file or cached value. This boot id will
 * not be as recent as the one stored in the system file if the system has been
 * rebooted since the last time we wrote to the metadata file. If the metadata
 * file doesn't exist, or another I/O error occurs, this will return FALSE and
 * set the error. Returns TRUE on success, and sets the boot_id out parameter to
 * the correct boot id.
 */
static gboolean
get_saved_boot_id (EmerPersistentCache *self,
                   uuid_t               boot_id,
                   GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (priv->boot_id_initialized)
    {
      uuid_copy (boot_id, priv->saved_boot_id);
      return TRUE;
    }

  gchar *id_as_string =
    g_key_file_get_string (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                           CACHE_LAST_BOOT_ID_KEY, error);
  if (id_as_string == NULL)
    {
      g_prefix_error (error, "Failed to read boot ID from %s. ",
                      priv->boot_metadata_file_path);
      return FALSE;
    }

  /*
   * A newline is appended when a string is stored in a keyfile.
   * We chomp it off here because uuid_parse will fail otherwise.
   */
  g_strchomp (id_as_string);
  if (uuid_parse (id_as_string, priv->saved_boot_id) != 0)
    {
      g_prefix_error (error, "Failed to parse saved boot ID: %s. ",
                      id_as_string);
      g_free (id_as_string);
      return FALSE;
    }

  g_free (id_as_string);
  uuid_copy (boot_id, priv->saved_boot_id);
  priv->boot_id_initialized = TRUE;
  return TRUE;
}

/*
 * Reads the operating system's boot id from disk or cached value, returning it
 * via the out parameter boot_id. Returns FALSE on failure and sets the GError.
 * Returns TRUE on success.
 */
static gboolean
get_system_boot_id (EmerPersistentCache *self,
                    uuid_t               boot_id,
                    GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (!emer_boot_id_provider_get_id (priv->boot_id_provider, boot_id))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Failed to get boot ID from EmerBootIdProvider.");
      return FALSE;
    }
  return TRUE;
}

/*
 * Creates and returns a newly allocated GFile * corresponding to the cache
 * file ending or metadata file ending given to it as path_ending.
 */
static GFile *
get_cache_file (EmerPersistentCache *self,
                gchar               *path_ending)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  gchar *path =
    g_strconcat (priv->cache_directory, CACHE_PREFIX, path_ending, NULL);
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
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GFile *ind_file = get_cache_file (self, INDIVIDUAL_SUFFIX);
  GError *local_error = NULL;
  gboolean success =
    g_file_replace_contents (ind_file, "", 0, NULL, FALSE,
                             G_FILE_CREATE_REPLACE_DESTINATION,
                             NULL, cancellable, &local_error);
  g_object_unref (ind_file);
  if (!success)
    goto handle_failed_write;

  GFile *agg_file = get_cache_file (self, AGGREGATE_SUFFIX);
  success =
    g_file_replace_contents (agg_file, "", 0, NULL, FALSE,
                             G_FILE_CREATE_REPLACE_DESTINATION,
                             NULL, cancellable, &local_error);
  g_object_unref (agg_file);
  if (!success)
    goto handle_failed_write;

  GFile *seq_file = get_cache_file (self, SEQUENCE_SUFFIX);
  success =
    g_file_replace_contents (seq_file, "", 0, NULL, FALSE,
                             G_FILE_CREATE_REPLACE_DESTINATION,
                             NULL, cancellable, &local_error);
  g_object_unref (seq_file);
  if (!success)
    goto handle_failed_write;

  priv->cache_size = 0L;
  priv->capacity = CAPACITY_LOW;

  return TRUE;

handle_failed_write:
  g_prefix_error (&local_error, "Failed to purge cache files. ");
  g_critical ("%s.", local_error->message);
  g_propagate_error (error, local_error);
  return FALSE;
}

/*
 * Will populate an already open GKeyFile with timing metadata and then write
 * that data to disk. Because all values for timestamps and offsets are
 * potentially valid, the parameters given must be the addresses of the data to
 * be written, or NULL to indicate that parameter should not be written.
 *
 * Returns FALSE if the GKeyFile fails to write to disk and sets the out GError
 * parameter, otherwise returns TRUE.
 */
static gboolean
save_timing_metadata (EmerPersistentCache *self,
                      const gint64        *relative_time_ptr,
                      const gint64        *absolute_time_ptr,
                      const gint64        *boot_offset_ptr,
                      const gchar         *boot_id_string,
                      const gboolean      *was_reset_ptr,
                      GError             **out_error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (relative_time_ptr != NULL)
    g_key_file_set_int64 (priv->boot_offset_key_file,
                          CACHE_TIMING_GROUP_NAME,
                          CACHE_RELATIVE_TIME_KEY,
                          *relative_time_ptr);

  if (absolute_time_ptr != NULL)
    g_key_file_set_int64 (priv->boot_offset_key_file,
                          CACHE_TIMING_GROUP_NAME,
                          CACHE_ABSOLUTE_TIME_KEY,
                          *absolute_time_ptr);

  if (boot_offset_ptr != NULL)
    g_key_file_set_int64 (priv->boot_offset_key_file,
                          CACHE_TIMING_GROUP_NAME,
                          CACHE_BOOT_OFFSET_KEY,
                          *boot_offset_ptr);

  if (boot_id_string != NULL)
    g_key_file_set_string (priv->boot_offset_key_file,
                           CACHE_TIMING_GROUP_NAME,
                           CACHE_LAST_BOOT_ID_KEY,
                           boot_id_string);

  if (was_reset_ptr != NULL)
    g_key_file_set_boolean (priv->boot_offset_key_file,
                            CACHE_TIMING_GROUP_NAME,
                            CACHE_WAS_RESET_KEY,
                            *was_reset_ptr);

  if (!g_key_file_save_to_file (priv->boot_offset_key_file,
                                priv->boot_metadata_file_path, out_error))
    {
      g_prefix_error (out_error, "Failed to write to metadata file: %s. ",
                      priv->boot_metadata_file_path);
      return FALSE;
    }

  return TRUE;
}

/*
 * Resets the boot timing metadata file to default values, completely replacing
 * any previously existing boot timing metadata file, if one even existed.
 *
 * Initializes the cache's boot offset and boot id.
 *
 * Completely wipes the persistent cache's stored metrics.
 *
 * Returns TRUE if successful and FALSE on failure.
 */
static gboolean
reset_boot_offset_metadata_file (EmerPersistentCache *self,
                                 gint64              *relative_time_ptr,
                                 gint64              *absolute_time_ptr)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->boot_offset_initialized = FALSE;
  priv->boot_id_initialized = FALSE;

  GError *error = NULL;
  uuid_t system_boot_id;
  if (!get_system_boot_id (self, system_boot_id, &error))
    {
      g_critical ("Failed to reset boot metadata. Error: %s.", error->message);
      g_error_free (error);
      return FALSE;
    }
  gchar system_boot_id_string[BOOT_ID_FILE_LENGTH];
  uuid_unparse_lower (system_boot_id, system_boot_id_string);

  if (!purge_cache_files (self, NULL, NULL))
    return FALSE;

  gint64 reset_offset = 0;
  gboolean was_reset = TRUE;
  gboolean write_success =
    save_timing_metadata (self, relative_time_ptr, absolute_time_ptr,
                          &reset_offset, system_boot_id_string, &was_reset,
                          &error);

  if (!write_success)
    {
      g_critical ("Failed to reset boot timing metadata. Error: %s.",
                   error->message);
      return FALSE;
    }

  priv->boot_offset = reset_offset;
  priv->boot_offset_initialized = TRUE;
  uuid_copy (priv->saved_boot_id, system_boot_id);
  priv->boot_id_initialized = TRUE;
  return TRUE;
}

/*
 * Takes an already open GKeyFile and cached timestamps and computes the
 * new and correct boot offset, storing it in the out parameter.
 * Returns TRUE if this is done successfully, and returns FALSE if there is an
 * error loading the stored timestamps from the metadata file, which are needed
 * to compute the correct boot offset.
 */
static gboolean
compute_boot_offset (EmerPersistentCache *self,
                     gint64               relative_time,
                     gint64               absolute_time,
                     gint64              *boot_offset)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GError *error = NULL;

  /*
   * This is the amount of time elapsed between the origin boot and the boot
   * with the stored ID.
   */
  gint64 stored_offset =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_BOOT_OFFSET_KEY, &error);
  if (error != NULL)
    {
      g_critical ("Failed to read boot offset from metadata file %s. "
                  "Error: %s.", priv->boot_metadata_file_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  gint64 stored_relative_time =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_RELATIVE_TIME_KEY, &error);
  if (error != NULL)
    {
      g_critical ("Failed to read relative time from metadata file %s. "
                  "Error: %s.", priv->boot_metadata_file_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  gint64 stored_absolute_time =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_ABSOLUTE_TIME_KEY, &error);
  if (error != NULL)
    {
      g_critical ("Failed to read absolute time from metadata file %s. "
                  "Error: %s.", priv->boot_metadata_file_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  /*
   * This is the amount of time elapsed between the origin boot and the boot
   * with the currently stored ID.
   */
  gint64 previous_offset = stored_offset;

  /*
   * This is the amount of time elapsed between the origin boot and the time at
   * which the stored file was written.
   */
  gint64 time_between_origin_boot_and_write =
    previous_offset + stored_relative_time;

  /*
   * This is our best estimate of the actual amount of time elapsed between the
   * most recent write to the store file and the current time.
   */
  gint64 approximate_time_since_last_write =
    absolute_time - stored_absolute_time;

  /*
   * This is our best estimate of the amount of time elapsed between the origin
   * boot and the current time.
   */
  gint64 time_since_origin_boot =
    time_between_origin_boot_and_write + approximate_time_since_last_write;

  /*
   * This is our best estimate of the amount of time elapsed between the origin
   * boot and the current boot. This is the new boot offset.
   */
  *boot_offset = time_since_origin_boot - relative_time;

  return TRUE;
}

/*
 * Will read and compute the boot offset from a boot offset file or will use a
 * cached value, if one is available. If the metadata file doesn't exist, it
 * will reset the cache metadata and purge the system. If an error occurs while
 * trying to read the boot id other than 'file not found', or if the timestamp
 * cannot be generated, this will return FALSE.
 *
 * Will attempt to update the timestamps in the metadata file, but it is not
 * considered a failure if it is unable to do so. Returns TRUE on success and
 * caches the boot offset and boot id in the EmerPersistentCache.
 *
 * The net effect of the entire system is that the most plausible way to trick
 * it is to adjust the system clock and yank the power cord before the next
 * network send occurs (an hourly event).
 *
 * Callers of this function need to acquire a lock on update_boot_offset before
 * calling this function and should free that lock immediately after it returns.
 */
static gboolean
update_boot_offset (EmerPersistentCache *self,
                    gboolean             always_update_timestamps)
{
  gint64 relative_time, absolute_time;
  if (!emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time) ||
      !emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time))
    {
      g_critical ("Could not get the boot offset because getting the current "
                  "time failed.");
      return FALSE;
    }

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GError *error = NULL;

  if (priv->boot_offset_initialized)
    {
      if (always_update_timestamps)
        {
          gboolean write_success =
            save_timing_metadata (self, &relative_time, &absolute_time,
                                  NULL, NULL, NULL, &error);
          if (!write_success)
            {
              g_warning ("Failed to update relative and absolute time in "
                         "metadata file. Error: %s.", error->message);
              g_error_free (error);
            }
        }

      return TRUE;
    }

  gboolean load_succeeded =
    g_key_file_load_from_file (priv->boot_offset_key_file,
                               priv->boot_metadata_file_path, G_KEY_FILE_NONE,
                               &error);
  if (!load_succeeded)
    {
      if (!g_error_matches (error, G_KEY_FILE_ERROR,
                            G_KEY_FILE_ERROR_NOT_FOUND) &&
          !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Got an unexpected error trying to load %s. Error: %s.",
                   priv->boot_metadata_file_path, error->message);

      g_error_free (error);
      return reset_boot_offset_metadata_file (self, &relative_time,
                                              &absolute_time);
    }

  gint64 boot_offset =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_BOOT_OFFSET_KEY, &error);
  if (error != NULL)
    {
      g_warning ("Failed to read boot offset from metadata file %s. "
                 "Error: %s.", priv->boot_metadata_file_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  uuid_t saved_boot_id, system_boot_id;
  if (!get_saved_boot_id (self, saved_boot_id, &error) ||
      !get_system_boot_id (self, system_boot_id, &error))
    {
      g_critical ("Failed to access boot ids for comparison. Error: %s.",
                  error->message);
      g_error_free (error);
      return FALSE;
    }

  if (uuid_compare (saved_boot_id, system_boot_id) == 0)
    {
      if (always_update_timestamps)
        {
          gboolean write_success =
            save_timing_metadata (self, &relative_time, &absolute_time,
                                  NULL, NULL, NULL, &error);

          if (!write_success)
            {
              g_warning ("Failed to update relative and absolute time in "
                         "metadata file. Error: %s.", error->message);
              g_error_free (error);
            }
        }

      return TRUE;
    }

  if (!compute_boot_offset (self, relative_time, absolute_time, &boot_offset))
    return FALSE;

  gchar system_boot_id_string[BOOT_ID_FILE_LENGTH];
  uuid_unparse_lower (system_boot_id, system_boot_id_string);

  gboolean was_reset = FALSE;
  gboolean write_success =
    save_timing_metadata (self, &relative_time, &absolute_time, &boot_offset,
                          system_boot_id_string, &was_reset, &error);

  if (!write_success)
    {
      g_warning ("Failed to write computed boot offset. Resetting cache. "
                 "Error: %s.", error->message);
      g_error_free (error);
      return FALSE;
    }

  priv->boot_offset = boot_offset;
  priv->boot_offset_initialized = TRUE;
  return TRUE;
}

/*
 * Callback for updating the boot offset file.
 */
static gboolean
update_boot_offset_source_func (EmerPersistentCache *self)
{
  G_LOCK (update_boot_offset);
  update_boot_offset (self, TRUE);
  G_UNLOCK (update_boot_offset);

  return G_SOURCE_CONTINUE;
}

/* Gets the boot time offset and stores it in the out parameter offset.
 * If the always_update_timestamps parameter is FALSE, does not write to disk
 * solely to update the timestamps during this operation unless the boot id is
 * out of date or some corruption is detected that prompts a total rewrite of
 * the boot timing metadata file. Pass an offset of NULL if you don't care about
 * its value.
 *
 * Returns TRUE on success. Returns FALSE on failure and sets the error unless
 * it's NULL. Does not alter the offset out parameter on failure.
 */
gboolean
emer_persistent_cache_get_boot_time_offset (EmerPersistentCache *self,
                                            gint64              *offset,
                                            GError             **error,
                                            gboolean             always_update_timestamps)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  G_LOCK (update_boot_offset);

  /* When always_update_timestamps is FALSE, the timestamps won't be written
   * unless the boot offset in the metadata file is being overwritten.
   */
  if (!update_boot_offset (self, always_update_timestamps))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Couldn't read boot offset.");
      G_UNLOCK (update_boot_offset);
      return FALSE;
    }

  G_UNLOCK (update_boot_offset);

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (offset != NULL)
    *offset = priv->boot_offset;

  return TRUE;
}

static GVariant *
deep_copy_variant (GVariant *variant)
{
  GBytes *bytes = g_variant_get_data_as_bytes (variant);
  const GVariantType *variant_type = g_variant_get_type (variant);
  GVariant *copy = g_variant_new_from_bytes (variant_type, bytes, TRUE);
  g_bytes_unref (bytes);
  return copy;
}

/* Return a new floating GVariant with the machine's native endianness that is
 * in normal form and marked trusted.
 */
static GVariant *
regularize_variant (GVariant *variant)
{
  g_variant_ref_sink (variant);
  GVariant *normalized_variant = g_variant_get_normal_form (variant);
  g_variant_unref (variant);

  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    {
      GVariant *byteswapped_variant = g_variant_byteswap (normalized_variant);
      g_variant_unref (normalized_variant);
      normalized_variant = byteswapped_variant;
    }
  else if (G_BYTE_ORDER != G_LITTLE_ENDIAN)
    {
      g_error ("This machine is neither big endian nor little endian. Mixed-"
               "endian machines are not supported by the metrics system.");
    }

  // Restore floating bit.
  GVariant *native_endian_variant = deep_copy_variant (normalized_variant);

  g_variant_unref (normalized_variant);
  return native_endian_variant;
}

/*
 * Will transfer all metrics in the corresponding file into the out parameter
 * 'return_list'. The list will be NULL-terminated. Returns TRUE on success, and
 * FALSE if any I/O error occured. Contents of return_list are undefined if the
 * return value is FALSE.
 */
static gboolean
drain_metrics_file (EmerPersistentCache *self,
                    GVariant          ***return_list,
                    gchar               *path_ending,
                    gchar               *variant_type)
{
  GError *error = NULL;
  GFile *file = get_cache_file (self, path_ending);
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

  GArray *dynamic_array = g_array_new (TRUE, FALSE, sizeof (GVariant *));
  g_array_set_clear_func (dynamic_array, (GDestroyNotify) g_variant_unref);

  while (TRUE)
    {
      guint64 little_endian_length;
      gsize length_bytes_read;
      gboolean read_succeeded =
        g_input_stream_read_all (stream,
                                 &little_endian_length,
                                 sizeof (little_endian_length),
                                 &length_bytes_read,
                                 NULL /* GCancellable */,
                                 &error);
      if (!read_succeeded)
        {
          g_critical ("Failed to read length of event in persistent cache. "
                      "Error: %s.", error->message);
          g_error_free (error);
          goto handle_failed_read;
        }

      if (length_bytes_read == 0) // EOF
        break;

      if (length_bytes_read != sizeof (little_endian_length))
        {
          g_critical ("Read %" G_GSIZE_FORMAT " bytes, but expected length of "
                      "event to be %" G_GSIZE_FORMAT " bytes.",
                      length_bytes_read, sizeof (little_endian_length));
          goto handle_failed_read;
        }

      guint64 variant_length =
        swap_bytes_64_if_big_endian (little_endian_length);
      gpointer variant_data = g_new (guchar, variant_length);
      gsize data_bytes_read;
      read_succeeded =
        g_input_stream_read_all (stream, variant_data, variant_length,
                                 &data_bytes_read, NULL /* GCancellable */,
                                 &error);
      if (!read_succeeded)
        {
          g_free (variant_data);
          g_critical ("Failed to read event in persistent cache. Error: %s.",
                      error->message);
          g_error_free (error);
          goto handle_failed_read;
        }

      if (data_bytes_read != variant_length)
        {
          g_free (variant_data);
          g_critical ("Cache file ended earlier than expected. Read %"
                      G_GSIZE_FORMAT " bytes, but expected %" G_GUINT64_FORMAT
                      " bytes of event data.", data_bytes_read, variant_length);
          goto handle_failed_read;
        }

      // Deserialize
      GVariant *current_event =
        g_variant_new_from_data (G_VARIANT_TYPE (variant_type), variant_data,
                                 variant_length, FALSE /* trusted */, g_free,
                                 variant_data);

      GVariant *regularized_event = regularize_variant (current_event);
      g_array_append_val (dynamic_array, regularized_event);
    }

  g_object_unref (stream);
  g_object_unref (file);

  *return_list = (GVariant **) g_array_free (dynamic_array, FALSE);

  return TRUE;

handle_failed_read:
  g_object_unref (stream);
  g_object_unref (file);
  g_array_unref (dynamic_array);
  return FALSE;
}

/*
 * Mutates the given out parameter GVariant pointer list to become a list of
 * GVariant pointers with the only element set to NULL. This is an empty,
 * NULL-terminated list.
 */
static void
set_to_empty_list (GVariant ***list)
{
  *list = g_new (GVariant *, 1);
  (*list)[0] = NULL;
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
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (priv->cache_size == 0)
    {
      set_to_empty_list (list_of_individual_metrics);
      set_to_empty_list (list_of_aggregate_metrics);
      set_to_empty_list (list_of_sequence_metrics);
      return TRUE;
    }

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
      free_variant_array (*list_of_individual_metrics);
      return FALSE;
    }

  gboolean seq_success = drain_metrics_file (self,
                                             list_of_sequence_metrics,
                                             SEQUENCE_SUFFIX,
                                             SEQUENCE_TYPE);
  if (!seq_success)
    {
      free_variant_array (*list_of_individual_metrics);
      free_variant_array (*list_of_aggregate_metrics);
      return FALSE;
    }

  if (!purge_cache_files (self, NULL, NULL))
    {
      free_variant_array (*list_of_individual_metrics);
      free_variant_array (*list_of_aggregate_metrics);
      free_variant_array (*list_of_sequence_metrics);
      return FALSE;
    }

  return TRUE;
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

  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (priv->cache_size_provider);
  return priv->cache_size + size <= max_cache_size;
}

/*
 * Returns a hint (capacity_t) as to how filled up the cache is and
 * updates the internal value of capacity. If 'set_to_max' is TRUE, will update
 * to (and return) CAPACITY_MAX.
 */
static capacity_t
update_capacity (EmerPersistentCache *self,
                 gboolean             set_to_max)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (set_to_max)
    {
      priv->capacity = CAPACITY_MAX;
    }
  else
    {
      guint64 max_cache_size =
        emer_cache_size_provider_get_max_cache_size (priv->cache_size_provider);

      if (priv->cache_size >= HIGH_CAPACITY_THRESHOLD * max_cache_size)
        priv->capacity = CAPACITY_HIGH;
      else
        priv->capacity = CAPACITY_LOW;
    }

  return priv->capacity;
}

/*
 * Serializes the given variant little-endian with its length in bytes as a
 * little-endian guint64 prepended to it. Appends the entire serialized blob to
 * the end of the given byte array.
 *
 * If there is not enough room in the persistent cache to store the given byte
 * array with the given variant appended, the given byte array is not mutated,
 * and the function returns FALSE. Otherwise, it will mutate it as described and
 * return TRUE.
 */
static gboolean
append_variant (EmerPersistentCache *self,
                GByteArray          *serialized_variants,
                GVariant            *variant)
{
  guint64 variant_length = g_variant_get_size (variant);
  gsize event_size_on_disk = sizeof (variant_length) + variant_length;
  gsize byte_array_size_on_disk = serialized_variants->len + event_size_on_disk;
  g_variant_ref_sink (variant);
  if (cache_has_room (self, byte_array_size_on_disk))
    {
      guint64 little_endian_length =
        swap_bytes_64_if_big_endian (variant_length);

      GVariant *native_endian_variant = swap_bytes_if_big_endian (variant);
      g_variant_unref (variant);

      gconstpointer serialized_variant =
        g_variant_get_data (native_endian_variant);

      g_byte_array_append (serialized_variants,
                           (const guint8 *) &little_endian_length,
                           sizeof (little_endian_length));
      g_byte_array_append (serialized_variants, serialized_variant,
                           variant_length);

      g_variant_unref (native_endian_variant);
      return TRUE;
    }

  g_variant_unref (variant);
  return FALSE;
}

/*
 * Appends the given byte array to the end of the given file. Updates the size
 * of the cache if the store was successful.
 *
 * Returns TRUE on success and FALSE on failure.
 */
static gboolean
write_byte_array (EmerPersistentCache *self,
                  GFile               *file,
                  GByteArray          *byte_array)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GError *error = NULL;
  GFileOutputStream *stream =
    g_file_append_to (file, G_FILE_CREATE_NONE, NULL, &error);
  if (stream == NULL)
    {
      g_critical ("Failed to open stream to cache file. Error: %s.",
                  error->message);
      g_error_free (error);
      return FALSE;
    }

  gboolean success =
    g_output_stream_write_all (G_OUTPUT_STREAM (stream), byte_array->data,
                               byte_array->len, NULL, NULL, &error);
  g_object_unref (stream);

  if (!success)
    {
      g_critical ("Failed to write to cache file. Error: %s.", error->message);
      g_error_free (error);
      return FALSE;
    }

  priv->cache_size += byte_array->len;
  return TRUE;
}

static GVariant *
replace_payload_with_copy (EventValue *event_value)
{
  GVariant *auxiliary_payload = event_value->auxiliary_payload;
  if (auxiliary_payload == NULL)
    return NULL;

  event_value->auxiliary_payload = deep_copy_variant (auxiliary_payload);
  return auxiliary_payload;
}

static void
consume_floating_ref (EventValue *event_value)
{
  GVariant *auxiliary_payload = event_value->auxiliary_payload;
  if (auxiliary_payload == NULL)
    return;

  g_variant_ref_sink (auxiliary_payload);
  g_variant_unref (auxiliary_payload);
}

static gboolean
store_singulars (EmerPersistentCache *self,
                 SingularEvent       *singular_buffer,
                 gint                 num_singulars_buffered,
                 gint                *num_singulars_stored,
                 capacity_t          *capacity)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GByteArray *serialized_variants = g_byte_array_new ();
  gboolean will_fit = TRUE;
  gint i;
  for (i = 0; i < num_singulars_buffered; i++)
    {
      SingularEvent *curr_singular = singular_buffer + i;
      EventValue *curr_event_value = &curr_singular->event_value;
      GVariant *curr_payload = replace_payload_with_copy (curr_event_value);
      GVariant *curr_singular_variant = singular_to_variant (curr_singular);
      curr_event_value->auxiliary_payload = curr_payload;
      will_fit =
        append_variant (self, serialized_variants, curr_singular_variant);
      if (!will_fit)
        break;
    }

  gboolean write_successful = TRUE;
  if (i > 0)
    {
      GFile *singulars_file = get_cache_file (self, INDIVIDUAL_SUFFIX);
      write_successful =
        write_byte_array (self, singulars_file, serialized_variants);
      g_object_unref (singulars_file);
    }

  g_byte_array_unref (serialized_variants);

  if (write_successful)
    {
      for (gint j = 0; j < i; j++)
        {
          SingularEvent *curr_singular = singular_buffer + j;
          EventValue *curr_event_value = &curr_singular->event_value;
          consume_floating_ref (curr_event_value);
        }

      *num_singulars_stored = i;
      *capacity = update_capacity (self, !will_fit);
    }
  else
    {
      *num_singulars_stored = 0;
      *capacity = priv->capacity;
    }

  return write_successful;
}

static gboolean
store_aggregates (EmerPersistentCache *self,
                  AggregateEvent      *aggregate_buffer,
                  gint                 num_aggregates_buffered,
                  gint                *num_aggregates_stored,
                  capacity_t          *capacity)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GByteArray *serialized_variants = g_byte_array_new ();
  gboolean will_fit = TRUE;
  gint i;
  for (i = 0; i < num_aggregates_buffered; i++)
    {
      AggregateEvent *curr_aggregate = aggregate_buffer + i;
      EventValue *curr_event_value = &curr_aggregate->event.event_value;
      GVariant *curr_payload = replace_payload_with_copy (curr_event_value);
      GVariant *curr_aggregate_variant = aggregate_to_variant (curr_aggregate);
      curr_event_value->auxiliary_payload = curr_payload;
      will_fit =
        append_variant (self, serialized_variants, curr_aggregate_variant);
      if (!will_fit)
        break;
    }

  gboolean write_successful = TRUE;
  if (i > 0)
    {
      GFile *aggregate_file = get_cache_file (self, AGGREGATE_SUFFIX);
      write_successful =
        write_byte_array (self, aggregate_file, serialized_variants);
      g_object_unref (aggregate_file);
    }

  g_byte_array_unref (serialized_variants);

  if (write_successful)
    {
      for (gint j = 0; j < i; j++)
        {
          AggregateEvent *curr_aggregate = aggregate_buffer + j;
          EventValue *curr_event_value = &curr_aggregate->event.event_value;
          consume_floating_ref (curr_event_value);
        }

      *num_aggregates_stored = i;
      *capacity = update_capacity (self, !will_fit);
    }
  else
    {
      *num_aggregates_stored = 0;
      *capacity = priv->capacity;
    }

  return write_successful;
}

static gboolean
store_sequences (EmerPersistentCache *self,
                 SequenceEvent       *sequence_buffer,
                 gint                 num_sequences_buffered,
                 gint                *num_sequences_stored,
                 capacity_t          *capacity)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GByteArray *serialized_variants = g_byte_array_new ();
  gboolean will_fit = TRUE;
  gint i;
  for (i = 0; i < num_sequences_buffered; i++)
    {
      SequenceEvent *curr_sequence = sequence_buffer + i;

      GVariant *curr_payloads[curr_sequence->num_event_values];
      for (gint j = 0; j < curr_sequence->num_event_values; j++)
        {
          EventValue *curr_event_value = curr_sequence->event_values + j;
          curr_payloads[j] = replace_payload_with_copy (curr_event_value);
        }

      GVariant *curr_sequence_variant = sequence_to_variant (curr_sequence);

      for (gint j = 0; j < curr_sequence->num_event_values; j++)
        {
          EventValue *curr_event_value = curr_sequence->event_values + j;
          curr_event_value->auxiliary_payload = curr_payloads[j];
        }

      will_fit =
        append_variant (self, serialized_variants, curr_sequence_variant);
      if (!will_fit)
        break;
    }

  gboolean write_successful = TRUE;
  if (i > 0)
    {
      GFile *sequences_file = get_cache_file (self, SEQUENCE_SUFFIX);
      write_successful =
        write_byte_array (self, sequences_file, serialized_variants);
      g_object_unref (sequences_file);
    }

  g_byte_array_unref (serialized_variants);

  if (write_successful)
    {
      for (gint j = 0; j < i; j++)
        {
          SequenceEvent *curr_sequence = sequence_buffer + j;
          for (gint k = 0; k < curr_sequence->num_event_values; k++)
            {
              EventValue *curr_event_value = curr_sequence->event_values + k;
              consume_floating_ref (curr_event_value);
            }
        }

      *num_sequences_stored = i;
      *capacity = update_capacity (self, !will_fit);
    }
  else
    {
      *num_sequences_stored = 0;
      *capacity = priv->capacity;
    }

  return write_successful;
}

/*
 * Will persistently store all metrics passed to it if doing so would not exceed
 * the persistent cache's space quota.
 * Will return the capacity of the cache via the out parameter 'capacity'.
 * Returns %TRUE on success, even if the metrics are intentionally dropped due
 * to space limitations. Returns %FALSE only on I/O error.
 *
 * Regardless of success or failure, num_singulars_stored,
 * num_aggregates_stored, num_sequences_stored, and capacity will be correctly
 * set.
 *
 * This function assumes all events given to it have already had their
 * relative timestamps corrected.
 */
gboolean
emer_persistent_cache_store_metrics (EmerPersistentCache  *self,
                                     SingularEvent        *singular_buffer,
                                     AggregateEvent       *aggregate_buffer,
                                     SequenceEvent        *sequence_buffer,
                                     gint                  num_singulars_buffered,
                                     gint                  num_aggregates_buffered,
                                     gint                  num_sequences_buffered,
                                     gint                 *num_singulars_stored,
                                     gint                 *num_aggregates_stored,
                                     gint                 *num_sequences_stored,
                                     capacity_t           *capacity)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  /*
   * Initialize the out parameters in case we return early. They may be updated
   * by store_singulars, store_aggregates, and store_sequences below.
   */
  *capacity = priv->capacity;
  *num_singulars_stored = 0;
  *num_aggregates_stored = 0;
  *num_sequences_stored = 0;

  gboolean singulars_stored =
    store_singulars (self, singular_buffer, num_singulars_buffered,
                     num_singulars_stored, capacity);
  if (!singulars_stored || *capacity == CAPACITY_MAX)
    return singulars_stored;

  gboolean aggregates_stored =
    store_aggregates (self, aggregate_buffer, num_aggregates_buffered,
                      num_aggregates_stored, capacity);
  if (!aggregates_stored || *capacity == CAPACITY_MAX)
    return aggregates_stored;

  /*
   * num_singulars_stored, num_aggregates_stored, num_sequences_stored, and
   * capacity may be updated within store_singulars, store_aggregates, and
   * store_sequences.
   */
  return store_sequences (self, sequence_buffer, num_sequences_buffered,
                          num_sequences_stored, capacity);
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
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GFile *singular_file = get_cache_file (self, INDIVIDUAL_SUFFIX);
  guint64 singular_disk_used;
  GError *local_error = NULL;
  gboolean success = g_file_measure_disk_usage (singular_file,
                                                G_FILE_MEASURE_REPORT_ANY_ERROR,
                                                NULL,
                                                NULL,
                                                NULL,
                                                &singular_disk_used,
                                                NULL,
                                                NULL,
                                                &local_error);
  g_object_unref (singular_file);
  if (!success)
    goto handle_failed_read;

  guint64 aggregate_disk_used;
  GFile *aggregate_file = get_cache_file (self, AGGREGATE_SUFFIX);
  success = g_file_measure_disk_usage (aggregate_file,
                                       G_FILE_MEASURE_REPORT_ANY_ERROR,
                                       NULL,
                                       NULL,
                                       NULL,
                                       &aggregate_disk_used,
                                       NULL,
                                       NULL,
                                       &local_error);
  g_object_unref (aggregate_file);
  if (!success)
    goto handle_failed_read;

  guint64 sequence_disk_used;
  GFile *sequence_file = get_cache_file (self, SEQUENCE_SUFFIX);
  success = g_file_measure_disk_usage (sequence_file,
                                       G_FILE_MEASURE_REPORT_ANY_ERROR,
                                       NULL,
                                       NULL,
                                       NULL,
                                       &sequence_disk_used,
                                       NULL,
                                       NULL,
                                       &local_error);
  g_object_unref (sequence_file);
  if (!success)
    goto handle_failed_read;

  priv->cache_size =
    singular_disk_used + aggregate_disk_used + sequence_disk_used;
  update_capacity (self, FALSE);
  return TRUE;

handle_failed_read:
  g_prefix_error (&local_error, "Failed to measure disk usage. ");
  g_critical ("%s.", local_error->message);
  g_propagate_error (error, local_error);
  return FALSE;
}

/*
 * Attempts to remove all cached metrics if the cache version file is out of date
 * or not found. Updates the cache version file as specified by the cache
 * provider if successful. Creates the cache directory if it doesn't exist.
 * Returns %TRUE on success and %FALSE on failure.
 */
static gboolean
apply_cache_versioning (EmerPersistentCache *self,
                        GCancellable        *cancellable,
                        GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (g_mkdir_with_parents (priv->cache_directory, 02774) != 0)
    {
      const gchar *err_str = g_strerror (errno);
      g_critical ("Failed to create directory: %s. Error: %s.",
                  priv->cache_directory, err_str);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create directory: %s. Error: %s.",
                   priv->cache_directory, err_str);
      return FALSE;
    }

  gint old_version;
  gboolean read_success =
    emer_cache_version_provider_get_version (priv->cache_version_provider,
                                             &old_version);

  if (!read_success || CURRENT_CACHE_VERSION != old_version)
    {
      GError *local_error = NULL;
      gboolean success = purge_cache_files (self, cancellable, &local_error);
      if (!success)
        {
          g_prefix_error (&local_error, "Will not update version number. ");
          g_critical ("%s.", local_error->message);
          g_propagate_error (error, local_error);
          return FALSE;
        }

      success =
        emer_cache_version_provider_set_version (priv->cache_version_provider,
                                                 CURRENT_CACHE_VERSION, error);
      if (!success)
        {
          g_prefix_error (&local_error, "Failed to update cache version number "
                          "to %i. ", CURRENT_CACHE_VERSION);
          g_critical ("%s.", local_error->message);
          g_propagate_error (error, local_error);
          return FALSE;
        }
    }

  return TRUE;
}

static void
set_cache_directory (EmerPersistentCache *self,
                     const gchar         *directory)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->cache_directory = g_strdup (directory);
}

static void
set_cache_size_provider (EmerPersistentCache   *self,
                         EmerCacheSizeProvider *cache_size_provider)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (cache_size_provider == NULL)
    priv->cache_size_provider = emer_cache_size_provider_new ();
  else
    priv->cache_size_provider = g_object_ref_sink (cache_size_provider);
}

static void
set_boot_id_provider (EmerPersistentCache *self,
                      EmerBootIdProvider  *boot_id_provider)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (boot_id_provider == NULL)
    priv->boot_id_provider = emer_boot_id_provider_new ();
  else
    priv->boot_id_provider = g_object_ref_sink (boot_id_provider);
}

static void
set_cache_version_provider (EmerPersistentCache      *self,
                            EmerCacheVersionProvider *cache_version_provider)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (cache_version_provider == NULL)
    priv->cache_version_provider = emer_cache_version_provider_new ();
  else
    priv->cache_version_provider = g_object_ref (cache_version_provider);
}

static void
set_boot_offset_update_interval (EmerPersistentCache *self,
                                 guint                seconds)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->boot_offset_update_timeout_source_id =
    g_timeout_add_seconds (seconds,
                           (GSourceFunc) update_boot_offset_source_func, self);
}

static void
emer_persistent_cache_constructed (GObject *object)
{
  EmerPersistentCache *self = EMER_PERSISTENT_CACHE (object);
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->boot_metadata_file_path =
    g_strconcat (priv->cache_directory, BOOT_OFFSET_METADATA_FILE, NULL);

  G_OBJECT_CLASS (emer_persistent_cache_parent_class)->constructed (object);
}

static void
emer_persistent_cache_set_property (GObject      *object,
                                    guint         property_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  EmerPersistentCache *self = EMER_PERSISTENT_CACHE (object);

  switch (property_id)
    {
    case PROP_CACHE_DIRECTORY:
      set_cache_directory (self, g_value_get_string (value));
      break;

    case PROP_CACHE_SIZE_PROVIDER:
      set_cache_size_provider (self, g_value_get_object (value));
      break;

    case PROP_BOOT_ID_PROVIDER:
      set_boot_id_provider (self, g_value_get_object (value));
      break;

    case PROP_CACHE_VERSION_PROVIDER:
      set_cache_version_provider (self, g_value_get_object (value));
      break;

    case PROP_BOOT_OFFSET_UPDATE_INTERVAL:
      set_boot_offset_update_interval (self, g_value_get_uint (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_persistent_cache_finalize (GObject *object)
{
  EmerPersistentCache *self = EMER_PERSISTENT_CACHE (object);
  update_boot_offset (self, TRUE);

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  g_source_remove (priv->boot_offset_update_timeout_source_id);

  g_object_unref (priv->cache_size_provider);
  g_object_unref (priv->boot_id_provider);
  g_object_unref (priv->cache_version_provider);
  g_free (priv->boot_metadata_file_path);
  g_key_file_unref (priv->boot_offset_key_file);
  g_free (priv->cache_directory);

  G_OBJECT_CLASS (emer_persistent_cache_parent_class)->finalize (object);
}

static void
emer_persistent_cache_class_init (EmerPersistentCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = emer_persistent_cache_constructed;
  object_class->set_property = emer_persistent_cache_set_property;
  object_class->finalize = emer_persistent_cache_finalize;

  /* Blurb string is good enough default documentation for this. */
  emer_persistent_cache_props[PROP_CACHE_DIRECTORY] =
    g_param_spec_string ("cache-directory", "Cache directory",
                         "The directory to save metrics and metadata in.",
                         PERSISTENT_CACHE_DIR,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this. */
  emer_persistent_cache_props[PROP_CACHE_SIZE_PROVIDER] =
    g_param_spec_object ("cache-size-provider", "Cache size provider",
                         "The provider for the maximum number of bytes that "
                         "may be stored in the persistent cache.",
                         EMER_TYPE_CACHE_SIZE_PROVIDER,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this. */
  emer_persistent_cache_props[PROP_BOOT_ID_PROVIDER] =
    g_param_spec_object ("boot-id-provider", "Boot id provider",
                         "The provider for the system boot id used to establish"
                         " whether the current boot is the same as the previous"
                         " boot encountered when last writing to the Persistent"
                         " Cache.",
                         EMER_TYPE_BOOT_ID_PROVIDER,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this. */
  emer_persistent_cache_props[PROP_CACHE_VERSION_PROVIDER] =
    g_param_spec_object ("cache-version-provider", "Cache version provider",
                         "The provider for the version of the local cache's"
                         " format.",
                         EMER_TYPE_CACHE_VERSION_PROVIDER,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this. */
  emer_persistent_cache_props[PROP_BOOT_OFFSET_UPDATE_INTERVAL] =
    g_param_spec_uint ("boot-offset-update-interval", "Boot offset update interval",
                       "The number of seconds between each automatic update"
                       " of the boot offset metadata file.",
                       0, G_MAXUINT, DEFAULT_BOOT_TIMESTAMPS_UPDATE,
                       G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_persistent_cache_props);
}

static void
emer_persistent_cache_init (EmerPersistentCache *self)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->cache_size = 0L;
  priv->capacity = CAPACITY_LOW;
  priv->boot_offset_key_file = g_key_file_new ();
}

static gboolean
emer_persistent_cache_initable_init (GInitable    *self,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  EmerPersistentCache *persistent_cache = EMER_PERSISTENT_CACHE (self);

  if (!apply_cache_versioning (persistent_cache, cancellable, error))
    return FALSE;

  G_LOCK (update_boot_offset);
  if (!update_boot_offset (persistent_cache, FALSE))
    {
      g_critical ("Couldn't update the boot offset upon initialization.");
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't update the boot offset upon initialization.");
      G_UNLOCK (update_boot_offset);
      return FALSE;
    }
  G_UNLOCK (update_boot_offset);

  return load_cache_size (persistent_cache, cancellable, error);
}

static void
emer_persistent_cache_initable_iface_init (GInitableIface *iface)
{
  iface->init = emer_persistent_cache_initable_init;
}

/* Returns a new persistent cache with the default configuration.
 */
EmerPersistentCache *
emer_persistent_cache_new (GCancellable *cancellable,
                           GError      **error)
{
  return g_initable_new (EMER_TYPE_PERSISTENT_CACHE, cancellable, error, NULL);
}

/* Returns a customized persistent cache. Use emer_persistent_cache_new to use
 * the default configuration.
 */
EmerPersistentCache *
emer_persistent_cache_new_full (GCancellable             *cancellable,
                                GError                  **error,
                                const gchar              *custom_directory,
                                EmerCacheSizeProvider    *cache_size_provider,
                                EmerBootIdProvider       *boot_id_provider,
                                EmerCacheVersionProvider *cache_version_provider,
                                guint                     boot_offset_update_interval)
{
  return g_initable_new (EMER_TYPE_PERSISTENT_CACHE,
                         cancellable,
                         error,
                         "cache-directory", custom_directory,
                         "cache-size-provider", cache_size_provider,
                         "boot-id-provider", boot_id_provider,
                         "cache-version-provider", cache_version_provider,
                         "boot-offset-update-interval", boot_offset_update_interval,
                         NULL);
}
