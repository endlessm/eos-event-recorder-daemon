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

#include <errno.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <glib-object.h>

#include <eosmetrics/eosmetrics.h>

#include "emer-circular-file.h"
#include "shared/metrics-util.h"

#define VARIANT_FILENAME "variants.dat"
#define DEFAULT_VERSION_FILENAME "local_version_file"

/* SECTION:emer-persistent-cache.c
 * @title: Persistent Cache
 * @short_description: A general-purpose persistent FIFO store for variants
 *
 * Uses a machine-independent storage format that may only be modified in a
 * backwards-incompatible manner when CURRENT_CACHE_VERSION is incremented.
 */

typedef struct _EmerPersistentCachePrivate
{
  EmerCacheSizeProvider *cache_size_provider;
  EmerBootIdProvider *boot_id_provider;
  EmerCacheVersionProvider *cache_version_provider;
  EmerCircularFile *variant_file;

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

/* If this version is greater than the version of the persisted variants,
 * they will be removed, and the file in which the version number is stored
 * will be updated.
 */
#define CURRENT_CACHE_VERSION 4

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
 * same boot the persistent cache was last initialized in.
 */
#define SYSTEM_BOOT_ID_FILE "/proc/sys/kernel/random/boot_id"

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
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to parse saved boot ID: %s", id_as_string);
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
                   "Failed to get boot ID from EmerBootIdProvider");
      return FALSE;
    }

  return TRUE;
}

static void
unlink_old_file (EmerPersistentCache *self,
                 const gchar         *filename)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  gchar *path = g_build_filename (priv->cache_directory, filename, NULL);
  if (g_unlink (path) != 0)
    {
      const gchar *error_string = g_strerror (errno);
      g_warning ("Failed to unlink old cache file %s. Error: %s.",
                 path, error_string);
    }
  g_free (path);
}

static void
unlink_old_files (EmerPersistentCache *self)
{
  unlink_old_file (self, "cache_individual.metrics");
  unlink_old_file (self, "cache_aggregate.metrics");
  unlink_old_file (self, "cache_sequence.metrics");
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
                      GError             **error)
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
                                priv->boot_metadata_file_path, error))
    {
      g_prefix_error (error, "Failed to write to metadata file: %s. ",
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
 * Completely wipes the persistent cache.
 *
 * Returns TRUE if successful and FALSE on failure.
 */
static gboolean
reset_boot_offset_metadata_file (EmerPersistentCache *self,
                                 gint64              *relative_time_ptr,
                                 gint64              *absolute_time_ptr,
                                 GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->boot_offset_initialized = FALSE;
  priv->boot_id_initialized = FALSE;

  uuid_t system_boot_id;
  if (!get_system_boot_id (self, system_boot_id, error))
    return FALSE;

  gchar system_boot_id_string[BOOT_ID_FILE_LENGTH];
  uuid_unparse_lower (system_boot_id, system_boot_id_string);

  if (!emer_circular_file_purge (priv->variant_file, error))
    return FALSE;

  gint64 reset_offset = 0;
  gboolean was_reset = TRUE;
  gboolean write_succeeded =
    save_timing_metadata (self, relative_time_ptr, absolute_time_ptr,
                          &reset_offset, system_boot_id_string, &was_reset,
                          error);

  if (!write_succeeded)
    return FALSE;

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
                     gint64              *boot_offset,
                     GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GError *local_error = NULL;

  /*
   * This is the amount of time elapsed between the origin boot and the boot
   * with the stored ID.
   */
  gint64 stored_offset =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_BOOT_OFFSET_KEY, &local_error);
  if (local_error != NULL)
    {
      g_propagate_prefixed_error (error, local_error,
                                  "Failed to read boot offset from metadata "
                                  "file %s. ",
                                  priv->boot_metadata_file_path);
      return FALSE;
    }

  gint64 stored_relative_time =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_RELATIVE_TIME_KEY, &local_error);
  if (local_error != NULL)
    {
      g_propagate_prefixed_error (error, local_error,
                                  "Failed to read relative time from metadata "
                                  "file %s. ",
                                  priv->boot_metadata_file_path);
      return FALSE;
    }

  gint64 stored_absolute_time =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_ABSOLUTE_TIME_KEY, &local_error);
  if (local_error != NULL)
    {
      g_propagate_prefixed_error (error, local_error,
                                  "Failed to read absolute time from metadata "
                                  "file %s. ",
                                  priv->boot_metadata_file_path);
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
 * timestamp update.
 */
static gboolean
update_boot_offset (EmerPersistentCache *self,
                    gboolean             always_update_timestamps,
                    GError             **error)
{
  gint64 relative_time, absolute_time;
  if (!emtr_util_get_current_time (CLOCK_BOOTTIME, &relative_time) ||
      !emtr_util_get_current_time (CLOCK_REALTIME, &absolute_time))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not get the "
                   "boot offset because getting the current time failed");
      return FALSE;
    }

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (priv->boot_offset_initialized)
    {
      if (always_update_timestamps)
        return save_timing_metadata (self, &relative_time, &absolute_time, NULL,
                                     NULL, NULL, error);

      return TRUE;
    }

  GError *local_error = NULL;
  gboolean load_succeeded =
    g_key_file_load_from_file (priv->boot_offset_key_file,
                               priv->boot_metadata_file_path, G_KEY_FILE_NONE,
                               &local_error);
  if (!load_succeeded)
    {
      if (!g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_propagate_error (error, local_error);
          return FALSE;
        }

      g_error_free (local_error);
      return reset_boot_offset_metadata_file (self, &relative_time,
                                              &absolute_time, error);
    }

  gint64 boot_offset =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_BOOT_OFFSET_KEY, &local_error);
  if (local_error != NULL)
    {
      g_propagate_prefixed_error (error, local_error,
                                  "Failed to read boot offset from metadata "
                                  "file %s. ",
                                  priv->boot_metadata_file_path);
      return FALSE;
    }

  uuid_t saved_boot_id, system_boot_id;
  if (!get_saved_boot_id (self, saved_boot_id, error) ||
      !get_system_boot_id (self, system_boot_id, error))
    return FALSE;

  if (uuid_compare (saved_boot_id, system_boot_id) == 0)
    {
      if (always_update_timestamps)
        {
          gboolean write_succeeded =
            save_timing_metadata (self, &relative_time, &absolute_time, NULL,
                                  NULL, NULL, error);
          if (!write_succeeded)
            return FALSE;
        }
    }
  else
    {
      gboolean compute_succeeded =
        compute_boot_offset (self, relative_time, absolute_time, &boot_offset,
                             error);
      if (!compute_succeeded)
        return FALSE;

      gchar system_boot_id_string[BOOT_ID_FILE_LENGTH];
      uuid_unparse_lower (system_boot_id, system_boot_id_string);

      gboolean was_reset = FALSE;
      gboolean write_succeeded =
        save_timing_metadata (self, &relative_time, &absolute_time,
                              &boot_offset, system_boot_id_string, &was_reset,
                              error);

      if (!write_succeeded)
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
  GError *error = NULL;
  if (!update_boot_offset (self, TRUE, &error))
    {
      g_warning ("Failed to update metadata timestamps. Error: %s",
                 error->message);
      g_error_free (error);
    }

  return G_SOURCE_CONTINUE;
}

/* Returns a new variant or a new reference to the given variant that is
 * little-endian, in normal form, and marked trusted. Free the return value with
 * g_variant_unref.
 */
static GVariant *
regularize_pre_storage (GVariant *variant)
{
  if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
    return g_variant_get_normal_form (variant);

  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    g_error ("This machine is neither big endian nor little endian. Mixed-"
             "endian machines are not supported.");

  /* Avoid sinking floating reference of variant. */
  GVariant *copy = deep_copy_variant (variant);
  g_variant_ref_sink (copy);
  GVariant *little_endian_variant = g_variant_byteswap (copy);
  g_variant_unref (copy);

  return little_endian_variant;
}

/* Returns a new floating variant with the machine's native endianness that is
 * in normal form and marked trusted.
 */
static GVariant *
regularize_post_storage (GVariant *variant)
{
  if (!g_variant_is_normal_form (variant))
    g_error ("A variant was stored in normal form but read in non-normal "
             "form.");

  if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
    return variant;

  if (G_BYTE_ORDER != G_BIG_ENDIAN)
    g_error ("This machine is neither big endian nor little endian. Mixed-"
             "endian machines are not supported.");

  g_variant_ref_sink (variant);
  GVariant *native_endian_variant = g_variant_byteswap (variant);
  g_variant_unref (variant);

  /* Restore floating bit. */
  GVariant *floating_variant = deep_copy_variant (native_endian_variant);

  g_variant_unref (native_endian_variant);
  return floating_variant;
}

/*
 * Attempts to wipe the persistent cache if the cache version file is out of
 * date or not found. Updates the cache version file as specified by the cache
 * provider if successful. Returns %TRUE on success and %FALSE on failure.
 */
static gboolean
apply_cache_versioning (EmerPersistentCache *self,
                        GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  gint old_version;
  gboolean read_succeeded =
    emer_cache_version_provider_get_version (priv->cache_version_provider,
                                             &old_version);

  if (!read_succeeded || CURRENT_CACHE_VERSION != old_version)
    {
      if (old_version < 4)
        unlink_old_files (self);

      if (!emer_circular_file_purge (priv->variant_file, error))
        {
          g_prefix_error (error, "Will not update version number. ");
          return FALSE;
        }

      gboolean set_succeeded =
        emer_cache_version_provider_set_version (priv->cache_version_provider,
                                                 CURRENT_CACHE_VERSION, error);
      if (!set_succeeded)
        {
          g_prefix_error (error,
                          "Failed to update cache version number to %i. ",
                          CURRENT_CACHE_VERSION);
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

  if (cache_version_provider != NULL)
    g_object_ref (cache_version_provider);
  priv->cache_version_provider = cache_version_provider;
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
    g_build_filename (priv->cache_directory, BOOT_OFFSET_METADATA_FILE, NULL);

  if (priv->cache_version_provider == NULL)
    {
      gchar *cache_version_path =
        g_build_filename (priv->cache_directory, DEFAULT_VERSION_FILENAME,
                          NULL);
      priv->cache_version_provider =
        emer_cache_version_provider_new (cache_version_path);
      g_free (cache_version_path);
    }

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
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GError *error = NULL;
  if (priv->variant_file != NULL && !update_boot_offset (self, TRUE, &error))
    {
      g_warning ("Failed to update boot offset when persistent cache was "
                 "finalized. Error: %s", error->message);
      g_error_free (error);
    }

  g_clear_pointer (&priv->boot_offset_update_timeout_source_id,
                   g_source_remove);

  g_clear_object (&priv->cache_size_provider);
  g_clear_object (&priv->boot_id_provider);
  g_clear_object (&priv->cache_version_provider);
  g_clear_object (&priv->variant_file);
  g_clear_pointer (&priv->boot_metadata_file_path, g_free);
  g_clear_pointer (&priv->boot_offset_key_file, g_key_file_unref);
  g_clear_pointer (&priv->cache_directory, g_free);

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
                         "The directory to save data in.",
                         NULL,
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

  priv->boot_offset_key_file = g_key_file_new ();
}

static gboolean
emer_persistent_cache_initable_init (GInitable    *initable,
                                     GCancellable *cancellable,
                                     GError      **error)
{
  EmerPersistentCache *self = EMER_PERSISTENT_CACHE (initable);
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (g_mkdir_with_parents (priv->cache_directory, 02775) != 0)
    {
      gint error_code = errno;
      GIOErrorEnum gio_error_code = g_io_error_from_errno (error_code);
      const gchar *error_string = g_strerror (error_code);
      g_set_error (error, G_IO_ERROR, gio_error_code,
                   "Failed to create directory: %s. Error: %s",
                   priv->cache_directory, error_string);
      return FALSE;
    }

  gchar *variant_file_path =
    g_build_filename (priv->cache_directory, VARIANT_FILENAME, NULL);
  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (priv->cache_size_provider);
  priv->variant_file =
    emer_circular_file_new (variant_file_path, max_cache_size, error);
  g_free (variant_file_path);
  if (priv->variant_file == NULL)
    return FALSE;

  if (!apply_cache_versioning (self, error))
    goto handle_failed_init;

  if (!update_boot_offset (self, FALSE, error))
    goto handle_failed_init;

  return TRUE;

handle_failed_init:
  g_clear_object (&priv->variant_file);
  return FALSE;
}

static void
emer_persistent_cache_initable_iface_init (GInitableIface *iface)
{
  iface->init = emer_persistent_cache_initable_init;
}

/* Returns a new persistent cache with the default configuration.
 */
EmerPersistentCache *
emer_persistent_cache_new (const gchar *directory,
                           GError     **error)
{
  return g_initable_new (EMER_TYPE_PERSISTENT_CACHE,
                         NULL /* GCancellable */,
                         error,
                         "cache-directory", directory,
                         NULL);
}

/* Returns a customized persistent cache. Use emer_persistent_cache_new to use
 * the default configuration.
 */
EmerPersistentCache *
emer_persistent_cache_new_full (const gchar              *directory,
                                EmerCacheSizeProvider    *cache_size_provider,
                                EmerBootIdProvider       *boot_id_provider,
                                EmerCacheVersionProvider *cache_version_provider,
                                guint                     boot_offset_update_interval,
                                GError                  **error)
{
  return g_initable_new (EMER_TYPE_PERSISTENT_CACHE,
                         NULL /* GCancellable */,
                         error,
                         "cache-directory", directory,
                         "cache-size-provider", cache_size_provider,
                         "boot-id-provider", boot_id_provider,
                         "cache-version-provider", cache_version_provider,
                         "boot-offset-update-interval", boot_offset_update_interval,
                         NULL);
}

/* Returns the cost of the given variant, which is the size of its serialized
 * representation in the persistent cache. See emer_persistent_cache_read for
 * more details.
 */
gsize
emer_persistent_cache_cost (GVariant *variant)
{
  const GVariantType *variant_type = g_variant_get_type (variant);
  gsize type_string_length =
    g_variant_type_get_string_length (variant_type) + 1;
  gsize variant_size = g_variant_get_size (variant);
  return type_string_length + variant_size;
}

/* Gets the boot time offset and stores it in the out parameter offset. Pass
 * an offset of NULL if you don't care about its value.
 *
 * Returns TRUE on success. Returns FALSE on failure and sets the error unless
 * it's NULL. Does not alter the offset out parameter on failure.
 */
gboolean
emer_persistent_cache_get_boot_time_offset (EmerPersistentCache *self,
                                            gint64              *offset,
                                            GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  gboolean update_succeeded =
    update_boot_offset (self, FALSE, error);
  if (!update_succeeded)
    return FALSE;

  if (offset != NULL)
    *offset = priv->boot_offset;

  return TRUE;
}

/* Persistently stores the given variants. Sets num_variants_stored to the
 * number of variants that were actually stored. Returns TRUE on success even
 * if all of the given variants don't fit in the space allocated to the
 * persistent cache. Returns FALSE only on error.
 */
gboolean
emer_persistent_cache_store (EmerPersistentCache *self,
                             GVariant           **variants,
                             gsize                num_variants,
                             gsize               *num_variants_stored,
                             GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  gsize curr_variants_stored = 0;
  for (; curr_variants_stored < num_variants; curr_variants_stored++)
    {
      GVariant *curr_variant = variants[curr_variants_stored];
      GVariant *regularized_variant = regularize_pre_storage (curr_variant);

      const gchar *type_string = g_variant_get_type_string (regularized_variant);
      gsize type_length = strlen (type_string) + 1;
      gconstpointer variant_data = g_variant_get_data (regularized_variant);
      gsize variant_size = g_variant_get_size (regularized_variant);

      gsize elem_size = type_length + variant_size;
      guint8 elem[elem_size];
      memcpy (elem, type_string, type_length);
      memcpy (elem + type_length, variant_data, variant_size);
      g_variant_unref (regularized_variant);
      if (!emer_circular_file_append (priv->variant_file, elem, elem_size))
        break;
    }

  if (!emer_circular_file_save (priv->variant_file, error))
    return FALSE;

  for (gsize i = 0; i < curr_variants_stored; i++)
    {
      g_variant_ref_sink (variants[i]);
      g_variant_unref (variants[i]);
    }

  *num_variants_stored = curr_variants_stored;
  return TRUE;
}

/* Populates variants with a C array of variants that cost no more than the
 * given amount in total (as defined by emer_persistent_cache_cost). Variants
 * are read in the same order in which they were stored; in other words, the
 * persistent cache is FIFO. Sets token to an opaque value that may be passed to
 * emer_persistent_cache_remove to remove the variants that were read in a
 * particular call to emer_persistent_cache_read. Tokens may not be reused, and
 * any successful call to emer_persistent_cache_remove invalidates any
 * outstanding tokens. If no variants were read but the read succeeded, then
 * variants is set to NULL. Returns TRUE on success and FALSE on error.
 */
gboolean
emer_persistent_cache_read (EmerPersistentCache *self,
                            GVariant          ***variants,
                            gsize                cost,
                            gsize               *num_variants,
                            guint64             *token,
                            GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GBytes **elems;
  gsize num_elems;
  guint64 local_token;
  gboolean read_succeeded =
    emer_circular_file_read (priv->variant_file, &elems, cost, &num_elems,
                             &local_token, error);
  if (!read_succeeded)
    return FALSE;

  GVariant **local_variants = g_new (GVariant *, num_elems);
  for (gsize i = 0; i < num_elems; i++)
    {
      gsize elem_size;
      const gchar *elem_data = g_bytes_get_data (elems[i], &elem_size);
      if (elem_data == NULL)
        g_error ("An element had size 0.");

      const gchar *end_of_type = memchr (elem_data, '\0', elem_size);
      if (end_of_type == NULL)
        g_error ("An element did not contain a null byte indicating the end of "
                 "a variant type string.");

      if (!g_variant_type_string_is_valid (elem_data))
        g_error ("An element did not begin with a valid variant type string.");

      const GVariantType *variant_type = G_VARIANT_TYPE (elem_data);
      const gchar *start_of_variant = end_of_type + 1;
      gsize variant_size = (elem_data + elem_size) - start_of_variant;
      gpointer variant_data = g_memdup (start_of_variant, variant_size);
      GVariant *curr_variant =
        g_variant_new_from_data (variant_type, variant_data, variant_size,
                                 FALSE /* trusted */, g_free, variant_data);
      g_bytes_unref (elems[i]);
      local_variants[i] = regularize_post_storage (curr_variant);
    }

  g_free (elems);
  *variants = local_variants;
  *num_variants = num_elems;
  *token = local_token;
  return TRUE;
}

/* Returns TRUE if there would still be at least one variant remaining after a
 * successful call to emer_persistent_cache_remove with this token. Returns
 * FALSE if a successful call to emer_persistent_cache_remove with this token
 * would result in all of the variants that are currently in the persistent
 * cache being removed. Calling emer_persistent_cache_has_more does not
 * invalidate this or any other token, but passing invalid tokens to
 * emer_persistent_cache_has_more results in undefined behavior. A token value
 * of 0 may be passed to ascertain whether the persistent cache is currently
 * empty.
 */
gboolean
emer_persistent_cache_has_more (EmerPersistentCache *self,
                                guint64              token)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  return emer_circular_file_has_more (priv->variant_file, token);
}

/* Removes the variants that were read in the call to emer_persistent_cache_read
 * that produced the given token. Tokens may not be reused, and any successful
 * call to emer_persistent_cache_remove invalidates any outstanding tokens. A
 * token value of 0 indicates that no variants should be removed. Returns TRUE
 * on success and FALSE on error.
 */
gboolean
emer_persistent_cache_remove (EmerPersistentCache *self,
                              guint64              token,
                              GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  return emer_circular_file_remove (priv->variant_file, token, error);
}
