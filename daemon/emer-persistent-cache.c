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

#include "shared/metrics-util.h"

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

static gboolean   append_metric                          (EmerPersistentCache *self,
                                                          GFile               *file,
                                                          GVariant            *metric);

static gboolean   apply_cache_versioning                 (EmerPersistentCache *self,
                                                          GCancellable        *cancellable,
                                                          GError             **error);

static gboolean   cache_has_room                         (EmerPersistentCache *self,
                                                          gsize                size);

static gboolean   compute_boot_offset                    (EmerPersistentCache *self,
                                                          gint64               relative_time,
                                                          gint64               absolute_time,
                                                          gint64              *boot_offset);

static gboolean   drain_metrics_file                     (EmerPersistentCache *self,
                                                          GVariant          ***return_list,
                                                          gchar               *path_ending,
                                                          gchar               *variant_type);

static void       emer_persistent_cache_initable_init    (GInitableIface      *iface);

static gboolean   emer_persistent_cache_may_fail_init    (GInitable           *self,
                                                          GCancellable        *cancellable,
                                                          GError             **error);

static GVariant * flip_bytes_if_big_endian_machine       (GVariant            *variant);

static void       free_variant_list                      (GVariant           **list);

static GFile *    get_cache_file                         (gchar               *path_ending);

static gboolean   get_saved_boot_id                      (EmerPersistentCache *self,
                                                          uuid_t               boot_id,
                                                          GError             **error);

static gboolean   get_system_boot_id                     (EmerPersistentCache *self,
                                                          uuid_t               boot_id,
                                                          GError             **error);

static gboolean   load_cache_size                        (EmerPersistentCache *self,
                                                          GCancellable        *cancellable,
                                                          GError             **error);

static gboolean   load_local_cache_version               (gint64              *version);

static gboolean   purge_cache_files                      (EmerPersistentCache *self,
                                                          GCancellable        *cancellable,
                                                          GError             **error);

static gboolean   reset_boot_offset_metafile             (EmerPersistentCache *self,
                                                          gint64              *relative_time_ptr,
                                                          gint64              *absolute_time_ptr);

static gboolean   save_timing_metadata                   (EmerPersistentCache *self,
                                                          const gint64        *relative_time_ptr,
                                                          const gint64        *absolute_time_ptr,
                                                          const gint64        *boot_offset_ptr,
                                                          const gchar         *boot_id_string,
                                                          const gboolean      *was_reset_ptr,
                                                          GError              **error);

static void       set_boot_id_provider                   (EmerPersistentCache *self,
                                                          EmerBootIdProvider  *boot_id_provider);

static gboolean   store_aggregates                       (EmerPersistentCache *self,
                                                          AggregateEvent      *aggregate_buffer,
                                                          gint                 num_aggregates_buffered,
                                                          gint                *num_aggregates_stored,
                                                          capacity_t          *capacity);

static gboolean   store_event                            (EmerPersistentCache *self,
                                                          GFile               *file,
                                                          GVariant            *event,
                                                          capacity_t          *capacity);

static gboolean   store_singulars                        (EmerPersistentCache *self,
                                                          SingularEvent       *singular_buffer,
                                                          gint                 num_singulars_buffered,
                                                          gint                *num_singulars_stored,
                                                          capacity_t          *capacity);

static gboolean   store_sequences                        (EmerPersistentCache *self,
                                                          SequenceEvent       *sequence_buffer,
                                                          gint                 num_sequences_buffered,
                                                          gint                *num_sequences_stored,
                                                          capacity_t          *capacity);

static gboolean   update_boot_offset                     (EmerPersistentCache *self,
                                                          gboolean             always_update_timestamps);

static gboolean   update_cache_version_number            (GCancellable        *cancellable,
                                                          GError             **error);

static capacity_t update_capacity                        (EmerPersistentCache *self);

typedef struct GVariantWritable
{
  gsize length;
  gpointer data;
} GVariantWritable;

typedef struct _EmerPersistentCachePrivate
{
  EmerBootIdProvider *boot_id_provider;

  gchar *boot_metafile_path;

  gint64 boot_offset;
  gboolean boot_offset_initialized;

  uuid_t saved_boot_id;
  gboolean boot_id_initialized;

  GKeyFile *boot_offset_key_file;

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

#define PERSISTENT_CACHE_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), EMER_TYPE_PERSISTENT_CACHE, EmerPersistentCachePrivate))

/*
 * The largest amount of memory (in bytes) that the metrics cache may
 * occupy before incoming metrics are ignored.
 *
 * Should never be altered in production code!
 */
static gint MAX_CACHE_SIZE = 92160; // 90 kB

/*
 * The path to the file containing the boot-id used to determine if this is the
 * same boot as previous metrics were recorded in or not.
 */
#define SYSTEM_BOOT_ID_FILE "/proc/sys/kernel/random/boot_id"

/*
 * The directory metrics and their meta-file are saved to.
 * Is listed in all caps because it should be treated as though
 * it were immutable by production code.  Only testing code should
 * ever alter this variable.
 */
static gchar *CACHE_DIRECTORY = "/var/cache/metrics/";

enum {
  PROP_0,
  PROP_BOOT_ID_PROVIDER,
  NPROPS
};

static GParamSpec *emer_persistent_cache_props[NPROPS] = { NULL, };

static void
set_boot_id_provider (EmerPersistentCache *self,
                      EmerBootIdProvider  *boot_id_provider)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (priv->boot_id_provider == NULL)
    priv->boot_id_provider = emer_boot_id_provider_new ();
  else
    priv->boot_id_provider = g_object_ref_sink (boot_id_provider);
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
    case PROP_BOOT_ID_PROVIDER:
      set_boot_id_provider (self, g_value_get_object (value));
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

  g_object_unref (priv->boot_id_provider);
  g_free (priv->boot_metafile_path);
  g_key_file_unref (priv->boot_offset_key_file);

  G_OBJECT_CLASS (emer_persistent_cache_parent_class)->finalize (object);
}

static void
emer_persistent_cache_class_init (EmerPersistentCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = emer_persistent_cache_set_property;
  object_class->finalize = emer_persistent_cache_finalize;

  /* Blurb string is good enough default documentation for this. */
  emer_persistent_cache_props[PROP_BOOT_ID_PROVIDER] =
    g_param_spec_object ("boot-id-provider", "Boot id provider",
                         "The provider for the system boot id used to establish"
                         " whether the current boot is the same as the previous"
                         " boot encountered when last writing to the Persistent"
                         " Cache.",
                         EMER_TYPE_BOOT_ID_PROVIDER,
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
  priv->boot_metafile_path = g_strconcat (CACHE_DIRECTORY, BOOT_OFFSET_METAFILE,
                                          NULL);
  priv->boot_offset_key_file = g_key_file_new ();
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
 * Please use this in production instead of the testing constructor!
 */
EmerPersistentCache *
emer_persistent_cache_new (GCancellable *cancellable,
                           GError      **error)
{
  return g_initable_new (EMER_TYPE_PERSISTENT_CACHE, cancellable, error, NULL);
}

/*
 * You should use emer_persistent_cache_new() instead of this function.
 * Function should only be used in testing code, NOT in production code.
 * Should always use a custom directory.  A custom cache size may be specified,
 * but if set to 0, the production value will be used.
 */
EmerPersistentCache *
emer_persistent_cache_new_full (GCancellable       *cancellable,
                                GError            **error,
                                gchar              *custom_directory,
                                gint                custom_cache_size,
                                EmerBootIdProvider *boot_id_provider)
{
  MAX_CACHE_SIZE = custom_cache_size;
  CACHE_DIRECTORY = custom_directory;
  return g_initable_new (EMER_TYPE_PERSISTENT_CACHE,
                         cancellable,
                         error,
                         "boot-id-provider", boot_id_provider,
                         NULL);
}

/*
 * Gets the boot time offset and stores it in the out parameter offset.
 * If the always_update_timestamps parameter is FALSE, this will not perform
 * writes to disk to update the timestamps during this operation unless the boot
 * id is out of date, or some corruption is detected which forces a rewrite of
 * the boot timing metafile.
 *
 * Will return TRUE on success. Will return FALSE on failure, will not set the
 * offset in the out parameter and will set the GError if error is not NULL.
 */
gboolean
emer_persistent_cache_get_boot_time_offset (EmerPersistentCache *self,
                                            gint64              *offset,
                                            GError             **error,
                                            gboolean             always_update_timestamps)
{
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  // When always_update_timestamps is FALSE, the timestamps won't be written
  // unless the boot offset in the metadata file is being overwritten.
  if (!update_boot_offset (self, always_update_timestamps))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Couldn't read boot offset.");
      return FALSE;
    }

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  *offset = priv->boot_offset;
  return TRUE;
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
                                priv->boot_metafile_path, out_error))
    {
      g_prefix_error (out_error, "Failed to write to metafile: %s .",
                      priv->boot_metafile_path);
      return FALSE;
    }

  return TRUE;
}

/*
 * Will read the boot id from a metadata file or cached value. This boot id will
 * not be as recent as the one stored in the system file if the system has been
 * rebooted since the last time we wrote to the metafile. If that file
 * doesn't exist, or another I/O error occurs, this will return FALSE and set
 * the error. Returns TRUE on success, and sets the boot_id out parameter to the
 * correct boot id.
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

  if (!g_key_file_load_from_file (priv->boot_offset_key_file,
                                  priv->boot_metafile_path,
                                  G_KEY_FILE_NONE,
                                  error))
    {
      g_prefix_error (error, "Failed to open KeyFile at: %s .",
                      priv->boot_metafile_path);
      return FALSE;
    }

  gchar *id_as_string = g_key_file_get_string (priv->boot_offset_key_file,
                                               CACHE_TIMING_GROUP_NAME,
                                               CACHE_LAST_BOOT_ID_KEY,
                                               error);
  if (id_as_string == NULL)
    {
      g_prefix_error (error, "Failed to read boot_id from %s .",
                      priv->boot_metafile_path);
      return FALSE;
    }

  /* Strangely, with both the keyfile and the system file, a newline is appended
     and retrieved when a uuid is changed to a string and stored on disk.
     We chomp it off here because uuid_parse will fail otherwise. */
  g_strchomp (id_as_string);
  if (uuid_parse (id_as_string, priv->saved_boot_id) != 0)
    {
      g_prefix_error (error, "Failed to parse the saved boot id: %s.",
                      id_as_string);
      g_free (id_as_string);
      return FALSE;
    }

  uuid_copy (boot_id, priv->saved_boot_id);
  priv->boot_id_initialized = TRUE;
  return TRUE;
}

/*
 * Reads the Operating System's boot id from disk or cached value, returning it
 * via the out parameter boot_id.  Returns FALSE on failure and sets the GError.
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
                   "Failed to get the boot ID from the EmerBootIdProvider.");
      return FALSE;
    }
  return TRUE;
}

/*
 * Resets the boot timing metafile to default values, completely replacing any
 * previously existing boot timing metafile, if one even existed.
 *
 * Initializes the cache's boot offset and boot id.
 *
 * Completely wipes the persistent cache's stored metrics.
 *
 * Returns FALSE and writes nothing to disk on failure. Returns TRUE on success.
 */
static gboolean
reset_boot_offset_metafile (EmerPersistentCache *self,
                            gint64              *relative_time_ptr,
                            gint64              *absolute_time_ptr)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);
  priv->boot_offset_initialized = FALSE;
  priv->boot_id_initialized = FALSE;

  GFile *meta_file = g_file_new_for_path (priv->boot_metafile_path);

  // We only want to 'touch' the file; we don't need the stream.
  GError *error = NULL;
  GFileOutputStream *unused_stream =
    g_file_replace (meta_file, NULL, FALSE,
                    G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
                    NULL, &error);
  g_object_unref (meta_file);
  if (unused_stream == NULL)
    {
      g_critical ("Failed to create new meta file at %s . Error: %s.",
                  priv->boot_metafile_path, error->message);
      g_error_free (error);
      return FALSE;
    }
  g_object_unref (unused_stream);

  // Wipe persistent cache if we had to reset the timing metadata.
  if (!purge_cache_files (self, NULL, &error))
    {
      g_error_free (error); // Error already reported.
      return FALSE;
    }

  uuid_t system_boot_id;
  if (!get_system_boot_id (self, system_boot_id, &error))
    {
      g_critical ("Failed to reset boot metadata. Error: %s", error->message);
      g_error_free (error);
      return FALSE;
    }
  gchar system_boot_id_string[BOOT_ID_FILE_LENGTH];
  uuid_unparse_lower (system_boot_id, system_boot_id_string);

  gint64 reset_offset = 0;
  gboolean was_reset = TRUE;
  gboolean write_success =
    save_timing_metadata (self, relative_time_ptr, absolute_time_ptr,
                          &reset_offset, system_boot_id_string, &was_reset,
                          &error);

  if (!write_success)
    {
      g_critical ("Failed to reset boot timing metadata. Error: %s",
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
 * Will read and compute the boot offset from a boot offset file or will use a
 * cached value, if one is available. If the meta-data file doesn't exist, it
 * will reset the cache metadata and purge the system. If an error occurs while
 * trying to read the boot id other than 'file not found', or if the timestamp
 * cannot be generated, this will return FALSE.
 *
 * Will attempt to update the timestamps in the metafile, but it is not
 * considered a failure if it is unable to do so. Returns TRUE on success and
 * caches the boot offset and boot id in the EmerPersistentCache.
 *
 * The net effect of the entire system is that pretty much the only way to trick
 * it is to adjust the system clock and yank the power cord before the next
 * network send occurs (an hourly event).
 */
static gboolean
update_boot_offset (EmerPersistentCache *self,
                    gboolean             always_update_timestamps)
{
  gint64 relative_time, absolute_time;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_time) ||
      !get_current_time (CLOCK_REALTIME, &absolute_time))
    {
      g_critical ("Could not get the boot offset because getting the current "
                  "time failed.");
      return FALSE;
    }

  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  GError *error = NULL;
  if (!g_key_file_load_from_file (priv->boot_offset_key_file,
                                  priv->boot_metafile_path,
                                  G_KEY_FILE_NONE,
                                  &error))
    {
      if (!g_error_matches (error, G_KEY_FILE_ERROR,
                            G_KEY_FILE_ERROR_NOT_FOUND) &&
          !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_warning ("Got an unexpected error trying to load %s . Error: %s.",
                     priv->boot_metafile_path, error->message);
        }
      g_error_free (error);
      return reset_boot_offset_metafile (self, &relative_time, &absolute_time);
    }

  if (priv->boot_offset_initialized)
    {
      gboolean write_success = TRUE;
      if (always_update_timestamps)
        write_success = save_timing_metadata (self, &relative_time,
                                              &absolute_time, NULL, NULL, NULL,
                                              &error);

      if (!write_success)
        {
          g_warning ("Failed to update relative and absolute time on metafile. "
                     "Error: %s.", error->message);
          g_error_free (error);
        }

      return TRUE;
    }

  gint64 boot_offset = g_key_file_get_int64 (priv->boot_offset_key_file,
                                             CACHE_TIMING_GROUP_NAME,
                                             CACHE_BOOT_OFFSET_KEY, &error);
  if (error != NULL)
    {
      g_error_free (error);
      return reset_boot_offset_metafile (self, &relative_time, &absolute_time);
    }

  uuid_t saved_boot_id, system_boot_id;
  if (!get_saved_boot_id (self, saved_boot_id, &error) ||
      !get_system_boot_id (self, system_boot_id, &error))
    {
      g_critical ("Failed to access boot ids for comparison. Error: %s",
                  error->message);
      g_error_free (error);
      return FALSE;
    }

  if (uuid_compare (saved_boot_id, system_boot_id) == 0)
    {
      gboolean write_success = TRUE;
      if (always_update_timestamps)
        write_success = save_timing_metadata (self, &relative_time,
                                              &absolute_time, NULL, NULL, NULL,
                                              &error);

      if (!write_success)
        {
          g_warning ("Failed to update relative and absolute time on metafile. "
                     "Error: %s.", error->message);
          g_error_free (error);
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
      return reset_boot_offset_metafile (self, &relative_time, &absolute_time);
    }

  priv->boot_offset = boot_offset;
  priv->boot_offset_initialized = TRUE;
  return TRUE;
}

/*
 * Takes an already open GKeyFile and cached timestamps and computes the
 * new and correct boot offset, storing it in the out parameter.
 * Returns TRUE if this is done successfully, and returns FALSE if there is an
 * error loading the stored timestamps from the metafile, which are needed to
 * compute the correct boot offset.
 */
static gboolean
compute_boot_offset (EmerPersistentCache *self,
                     gint64               relative_time,
                     gint64               absolute_time,
                     gint64              *boot_offset)
{
  GError *error = NULL;
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  /* The amount of time elapsed between the origin boot and the boot with
     the stored ID. */
  gint64 stored_offset = g_key_file_get_int64 (priv->boot_offset_key_file,
                                               CACHE_TIMING_GROUP_NAME,
                                               CACHE_BOOT_OFFSET_KEY,
                                               &error);
  if (error != NULL)
    {
      g_critical ("Failed to read relative offset from metafile %s . "
                  "Error: %s.", priv->boot_metafile_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  gint64 stored_relative_time =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_RELATIVE_TIME_KEY, &error);
  if (error != NULL)
    {
      g_critical ("Failed to read relative time from metafile %s . "
                  "Error: %s.", priv->boot_metafile_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  gint64 stored_absolute_time =
    g_key_file_get_int64 (priv->boot_offset_key_file, CACHE_TIMING_GROUP_NAME,
                          CACHE_ABSOLUTE_TIME_KEY, &error);
  if (error != NULL)
    {
      g_critical ("Failed to read absolute time from metafile %s . "
                  "Error: %s.", priv->boot_metafile_path, error->message);
      g_error_free (error);
      return FALSE;
    }

  /* The amount of time elapsed between the origin boot and the boot with the
     currently stored ID. */
  gint64 previous_offset = stored_offset;

  /* The amount of time elapsed between the origin boot and the time at which
     the stored file was written. */
  gint64 time_between_origin_boot_and_write =
    previous_offset + stored_relative_time;

  /* Our best estimate of the actual amount of time elapsed between the most
     recent write to the store file and the current time. */
  gint64 approximate_time_since_last_write =
    absolute_time - stored_absolute_time;

  /* Our best estimate of the amount of time elapsed between the origin boot
     and the current time. */
  gint64 time_since_origin_boot =
    time_between_origin_boot_and_write + approximate_time_since_last_write;

  /* Our best estimate of the amount of time elapsed between the origin boot and
     the current boot. This is the new boot offset. */
  *boot_offset = time_since_origin_boot - relative_time;

  return TRUE;
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
      g_variant_ref_sink (current_metric);

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
 * Attempts to write an event to a given file.
 * Will update capacity given to it. Automatically increments priv->cache_size.
 * Returns %FALSE if the write fails on account of I/O error. Returns %TRUE
 * otherwise. Regardless of success or failure, capacity will be correctly set.
 */
static gboolean
store_event (EmerPersistentCache *self,
             GFile               *file,
             GVariant            *event,
             capacity_t          *capacity)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  gboolean success;

  gsize event_size_on_disk = sizeof (gsize) + g_variant_get_size (event);
  if (cache_has_room (self, event_size_on_disk))
    {
      success = append_metric (self, file, event);
      if (!success)
        g_critical ("Failed to write event.");
      else
        priv->cache_size += event_size_on_disk;
    }
  else
    {
      priv->capacity = CAPACITY_MAX;
      success = TRUE;
    }

  *capacity = update_capacity (self);
  return success;
}

static gboolean
store_singulars (EmerPersistentCache *self,
                 SingularEvent       *singular_buffer,
                 gint                 num_singulars_buffered,
                 gint                *num_singulars_stored,
                 capacity_t          *capacity)
{
  GFile *singulars_file = get_cache_file (INDIVIDUAL_SUFFIX);

  gboolean stored_singulars = TRUE;
  gint i;
  for (i = 0; i < num_singulars_buffered; i++)
    {
      SingularEvent *curr_singular = singular_buffer + i;
      GVariant *curr_singular_variant = singular_to_variant (curr_singular);
      stored_singulars =
        store_event (self, singulars_file, curr_singular_variant, capacity);
      if (!stored_singulars)
        break;
    }

  g_object_unref (singulars_file);
  *num_singulars_stored = i;
  return stored_singulars;
}

static gboolean
store_aggregates (EmerPersistentCache *self,
                  AggregateEvent      *aggregate_buffer,
                  gint                 num_aggregates_buffered,
                  gint                *num_aggregates_stored,
                  capacity_t          *capacity)
{
  GFile *aggregates_file = get_cache_file (AGGREGATE_SUFFIX);

  gboolean stored_aggregates = TRUE;
  gint i;
  for (i = 0; i < num_aggregates_buffered; i++)
    {
      AggregateEvent *curr_aggregate = aggregate_buffer + i;
      GVariant *curr_aggregate_variant = aggregate_to_variant (curr_aggregate);
      stored_aggregates =
        store_event (self, aggregates_file, curr_aggregate_variant, capacity);
      if (!stored_aggregates)
        break;
    }

  g_object_unref (aggregates_file);
  *num_aggregates_stored = i;
  return stored_aggregates;
}

static gboolean
store_sequences (EmerPersistentCache *self,
                 SequenceEvent       *sequence_buffer,
                 gint                 num_sequences_buffered,
                 gint                *num_sequences_stored,
                 capacity_t          *capacity)
{
  GFile *sequences_file = get_cache_file (SEQUENCE_SUFFIX);

  gboolean stored_sequences = TRUE;
  gint i;
  for (i = 0; i < num_sequences_buffered; i++)
    {
      SequenceEvent *curr_sequence = sequence_buffer + i;
      GVariant *curr_sequence_variant = sequence_to_variant (curr_sequence);
      stored_sequences =
        store_event (self, sequences_file, curr_sequence_variant, capacity);
      if (!stored_sequences)
        break;
    }

  g_object_unref (sequences_file);
  *num_sequences_stored = i;
  return stored_sequences;
}

/*
 * Will persistently store all metrics passed to it if doing so would not exceed
 * the persistent cache's space quota.
 * Will return the capacity of the cache via the out parameter 'capacity'.
 * Returns %TRUE on success, even if the metrics are intentionally dropped due
 * to space limitations.  Returns %FALSE only on I/O error.
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

  if (!update_boot_offset (self, TRUE)) // Always update timestamps.
    {
      g_critical ("Couldn't update the boot offset, dropping metrics.");
      return FALSE;
    }

  gboolean singulars_stored = store_singulars (self,
                                               singular_buffer,
                                               num_singulars_buffered,
                                               num_singulars_stored,
                                               capacity);
  if (!singulars_stored || *capacity == CAPACITY_MAX)
    return singulars_stored;

  gboolean aggregates_stored = store_aggregates (self,
                                                 aggregate_buffer,
                                                 num_aggregates_buffered,
                                                 num_aggregates_stored,
                                                 capacity);
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

  g_variant_ref_sink (metric);
  GVariant *native_endian_metric = flip_bytes_if_big_endian_machine (metric);
  g_variant_unref (metric);

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
 * Will return a GVariant * with the bytes in the opposite order if this
 * machine is big-endian.
 *
 * The returned GVariant * should have g_variant_unref() called on it when it is
 * no longer needed.
 */
static GVariant *
flip_bytes_if_big_endian_machine (GVariant *variant)
{
  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    return g_variant_byteswap (variant);

  if (G_BYTE_ORDER != G_LITTLE_ENDIAN)
    g_error ("Holy crap! This machine is neither big NOR little-endian, "
             "time to panic. AAHAHAHAHAH!");

  return g_variant_ref_sink (variant);
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
