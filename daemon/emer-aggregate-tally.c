/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2021 Endless OS Foundation, LLC */

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

#include "emer-aggregate-tally.h"

#include <gio/gio.h>

#define AGGREGATE_TIMER_VARIANT_TYPE "(vvuumv)"

struct _EmerAggregateTally
{
  GObject parent_instance;

  gchar *persistent_cache_directory;
};

G_DEFINE_TYPE (EmerAggregateTally, emer_aggregate_tally, G_TYPE_OBJECT)

enum {
  PROP_PERSISTENT_CACHE_DIRECTORY = 1,
  N_PROPS,
};

static GParamSpec *properties[N_PROPS] = { NULL, };

static gchar *
format_datetime_for_tally_type (GDateTime     *datetime,
                                EmerTallyType  tally_type)
{
  switch (tally_type)
    {
    case EMER_TALLY_DAILY_EVENTS:
      return g_date_time_format (datetime, "%Y-%m-%d");

    case EMER_TALLY_MONTHLY_EVENTS:
      return g_date_time_format (datetime, "%Y-%m");

    default:
      g_assert_not_reached ();
    }

  return NULL;
}

static void
ensure_folder_exists (EmerAggregateTally  *self,
                      const char          *path,
                      GError             **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GFile) file = NULL;

  file = g_file_new_for_path (path);
  g_file_make_directory_with_parents (file, NULL, &local_error);

  if (local_error && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_propagate_error (error, g_steal_pointer (&local_error));
}

static inline GVariant *
aggregate_timer_to_variant (guint32    unix_user_id,
                            GVariant  *event_id,
                            GVariant  *aggregate_key,
                            GVariant  *payload,
                            guint32    counter,
                            gint64     monotonic_time_us)
{
  return g_variant_new (AGGREGATE_TIMER_VARIANT_TYPE,
                        event_id,
                        aggregate_key,
                        unix_user_id,
                        counter,
                        payload);
}

static gchar *
get_tally_path_from_aggregate_timer (EmerAggregateTally *self,
                                     guint32             unix_user_id,
                                     GVariant           *event_id,
                                     GVariant           *aggregate_key,
                                     const char         *date)
{
  g_autoptr(GChecksum) checksum = NULL;
  g_autofree gchar *aggregate_key_str = NULL;
  g_autofree gchar *event_id_str = NULL;

  aggregate_key_str = g_variant_print (aggregate_key, FALSE);
  event_id_str = g_variant_print (event_id, FALSE);

  checksum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (checksum,
                     (const guchar*) event_id_str,
                     strlen (event_id_str));
  g_checksum_update (checksum,
                     (const guchar*) &unix_user_id,
                     sizeof (guint32));
  g_checksum_update (checksum,
                     (const guchar*) aggregate_key_str,
                     strlen (aggregate_key_str));

  return g_build_filename (self->persistent_cache_directory,
                           "aggregate-timers",
                           date,
                           g_checksum_get_string (checksum),
                           NULL);
}

static GVariant*
load_aggregate_timer_from_path (const char *path)
{
  g_autoptr(GMappedFile) mapped_file = NULL;
  g_autoptr(GVariant) variant = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GError) error = NULL;

  mapped_file = g_mapped_file_new (path, FALSE, &error);
  if (error)
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning ("Error loading aggregate timer: %s", error->message);
      return NULL;
    }

  bytes = g_mapped_file_get_bytes (mapped_file);
  variant = g_variant_new_from_bytes (G_VARIANT_TYPE (AGGREGATE_TIMER_VARIANT_TYPE),
                                      bytes,
                                      FALSE);

  if (!g_variant_is_normal_form (variant))
    {
      g_autoptr(GError) deletion_error = NULL;
      g_autoptr(GFile) file = NULL;

      g_warning ("Corrupted aggregate timer at %s, deleting...", path);

      file = g_file_new_for_path (path);
      g_file_delete (file, NULL, &deletion_error);
      if (deletion_error)
        g_warning ("Error deleting corrupted tally file %s: %s",
                   path, deletion_error->message);
      return NULL;
    }

  return g_steal_pointer (&variant);
}

static guint32
find_previous_aggregate_timer_counter (EmerAggregateTally *self,
                                       const char         *aggregate_counter_path)
{
  g_autoptr(GVariant) variant = NULL;
  guint32 previous_counter = 0;

  variant = load_aggregate_timer_from_path (aggregate_counter_path);

  if (variant)
    g_variant_get_child (variant, 3, "u", &previous_counter);

  return previous_counter;
}

static void
emer_aggregate_tally_finalize (GObject *object)
{
  EmerAggregateTally *self = (EmerAggregateTally *)object;

  g_clear_pointer (&self->persistent_cache_directory, g_free);

  G_OBJECT_CLASS (emer_aggregate_tally_parent_class)->finalize (object);
}

static void
emer_aggregate_tally_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  EmerAggregateTally *self = EMER_AGGREGATE_TALLY (object);

  switch (prop_id)
    {
    case PROP_PERSISTENT_CACHE_DIRECTORY:
      g_assert (self->persistent_cache_directory == NULL);
      self->persistent_cache_directory = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
emer_aggregate_tally_class_init (EmerAggregateTallyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = emer_aggregate_tally_finalize;
  object_class->set_property = emer_aggregate_tally_set_property;

  /*
   * EmerDaemon:persistent-cache-directory:
   *
   * A directory for temporarily storing events until they are uploaded to the
   * metrics servers.
   */
  properties[PROP_PERSISTENT_CACHE_DIRECTORY] =
    g_param_spec_string ("persistent-cache-directory",
                         "Persistent cache directory",
                         "The directory in which to temporarily store events "
                         "locally",
                         NULL,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
emer_aggregate_tally_init (EmerAggregateTally *self)
{
}

EmerAggregateTally *
emer_aggregate_tally_new (const char *persistent_cache_directory)
{
  return g_object_new (EMER_TYPE_AGGREGATE_TALLY,
                       "persistent-cache-directory", persistent_cache_directory,
                       NULL);
}

gboolean
emer_aggregate_tally_store_event (EmerAggregateTally  *self,
                                  EmerTallyType        tally_type,
                                  guint32              unix_user_id,
                                  GVariant            *event_id,
                                  GVariant            *aggregate_key,
                                  GVariant            *payload,
                                  guint32              counter,
                                  GDateTime           *datetime,
                                  gint64               monotonic_time_us,
                                  GError             **error)
{
  g_autofree gchar *tally_path = NULL;
  g_autofree gchar *dirname = NULL;
  g_autofree gchar *date = NULL;
  g_autoptr(GVariant) timer_variant = NULL;
  g_autoptr(GError) local_error = NULL;
  guint32 previous_counter;

  date = format_datetime_for_tally_type (datetime, tally_type);
  tally_path = get_tally_path_from_aggregate_timer (self, unix_user_id,
                                                    event_id, aggregate_key,
                                                    date);

  /* It might happen that the same application was opened and closed
   * multiple times at the same day by the same user. In this case,
   * we want to increase the day's counter instead of simply override
   * it.
   */
  previous_counter = find_previous_aggregate_timer_counter (self, tally_path);

  dirname = g_path_get_dirname (tally_path);
  ensure_folder_exists (self, dirname, &local_error);
  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      g_prefix_error (error, "Error saving aggregate timer to tally: ");
      return FALSE;
    }

  timer_variant = aggregate_timer_to_variant (unix_user_id, event_id,
                                              aggregate_key, payload,
                                              counter + previous_counter,
                                              monotonic_time_us);

  g_file_set_contents (tally_path,
                       g_variant_get_data (timer_variant),
                       g_variant_get_size (timer_variant),
                       &local_error);

  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      g_prefix_error (error, "Error saving aggregate timer to tally: ");
      return FALSE;
    }

  return TRUE;
}

void
emer_aggregate_tally_iter (EmerAggregateTally *self,
                           EmerTallyType       tally_type,
                           GDateTime          *datetime,
                           EmerTallyIterFlags  flags,
                           EmerTallyIterFunc   func,
                           gpointer            user_data)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) tallies_folder = NULL;
  g_autofree gchar *tallies_path = NULL;
  g_autofree gchar *date = NULL;

  date = format_datetime_for_tally_type (datetime, tally_type);
  tallies_path = g_build_filename (self->persistent_cache_directory,
                                   "aggregate-timers",
                                   date,
                                   NULL);

  tallies_folder = g_file_new_for_path (tallies_path);

  enumerator = g_file_enumerate_children (tallies_folder,
                                          "standard::*",
                                          G_FILE_QUERY_INFO_NONE,
                                          NULL,
                                          &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_warning ("Error listing tallies from %s: %s",
                   tallies_path, error->message);
      return;
    }

  while (TRUE)
    {
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) event_id = NULL;
      g_autoptr(GVariant) aggregate_key = NULL;
      g_autoptr(GVariant) payload = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autofree gchar *path = NULL;
      EmerTallyIterResult result;
      GFileInfo *info;
      GFile *file;
      guint32 counter = 0;
      guint32 unix_user_id = 0;

      g_file_enumerator_iterate (enumerator, &info, &file, NULL, &local_error);

      if (local_error)
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            g_warning ("Error listing tallies from %s: %s",
                       tallies_path, local_error->message);
          return;
        }

      if (!file || !info)
        break;

      if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
        continue;

      path = g_file_get_path (file);

      variant = load_aggregate_timer_from_path (path);
      if (!variant)
        continue;

      g_variant_get (variant,
                     AGGREGATE_TIMER_VARIANT_TYPE,
                     &event_id,
                     &aggregate_key,
                     &unix_user_id,
                     &counter,
                     &payload);

      result = func (unix_user_id, event_id,
                     aggregate_key, payload,
                     counter, date, user_data);

      if (flags & EMER_TALLY_ITER_FLAG_DELETE)
        {
          g_autoptr(GError) deletion_error = NULL;

          if (!g_file_delete (file, NULL, &deletion_error))
            g_warning ("Error deleting tally file %s: %s",
                       path, deletion_error->message);
        }

      if (result & EMER_TALLY_ITER_STOP)
        break;
    }

  if (flags & EMER_TALLY_ITER_FLAG_DELETE)
    {
      g_autoptr(GError) deletion_error = NULL;

      if (!g_file_delete (tallies_folder, NULL, &deletion_error))
        g_warning ("Error deleting tally file %s: %s",
                   tallies_path, deletion_error->message);
    }
}
