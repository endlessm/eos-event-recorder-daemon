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
#include "mock-persistent-cache.h"

typedef struct _EmerPersistentCachePrivate
{
  gint num_timestamp_updates;
  gint store_metrics_called;
} EmerPersistentCachePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerPersistentCache, emer_persistent_cache,
                            G_TYPE_OBJECT);

static void
emer_persistent_cache_class_init (EmerPersistentCacheClass *klass)
{
}

static void
emer_persistent_cache_init (EmerPersistentCache *self)
{
}

EmerPersistentCache *
emer_persistent_cache_new (GCancellable *cancellable,
                           GError      **error)
{
  return g_object_new (EMER_TYPE_PERSISTENT_CACHE, NULL);
}

EmerPersistentCache *
emer_persistent_cache_new_full (GCancellable             *cancellable,
                                GError                  **error,
                                const gchar              *custom_directory,
                                guint64                   custom_cache_size,
                                EmerBootIdProvider       *boot_id_provider,
                                EmerCacheVersionProvider *version_provider,
                                guint                     boot_offset_update_interval)
{
  return g_object_new (EMER_TYPE_PERSISTENT_CACHE, NULL);
}

gboolean
emer_persistent_cache_get_boot_time_offset (EmerPersistentCache *self,
                                            gint64              *offset,
                                            GError             **error,
                                            gboolean             always_update_timestamps)
{
  if (always_update_timestamps)
    {
      EmerPersistentCachePrivate *priv =
        emer_persistent_cache_get_instance_private (self);
      priv->num_timestamp_updates++;
    }

  return TRUE;
}

gboolean
emer_persistent_cache_drain_metrics (EmerPersistentCache *self,
                                     GVariant          ***singular_array,
                                     GVariant          ***aggregate_array,
                                     GVariant          ***sequence_array,
                                     gint                 max_num_bytes)
{
  return TRUE;
}

gboolean
emer_persistent_cache_store_metrics (EmerPersistentCache *self,
                                     SingularEvent       *singular_buffer,
                                     AggregateEvent      *aggregate_buffer,
                                     SequenceEvent       *sequence_buffer,
                                     gint                 num_singulars_buffered,
                                     gint                 num_aggregates_buffered,
                                     gint                 num_sequences_buffered,
                                     gint                *num_singulars_stored,
                                     gint                *num_aggregates_stored,
                                     gint                *num_sequences_stored,
                                     capacity_t          *capacity)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  *num_singulars_stored = num_singulars_buffered;
  *num_aggregates_stored = num_aggregates_buffered;
  *num_sequences_stored = num_sequences_buffered;
  *capacity = CAPACITY_LOW;

  priv->store_metrics_called++;

  return TRUE;
}

/* API OF MOCK OBJECT */

gint
mock_persistent_cache_get_num_timestamp_updates (EmerPersistentCache *self)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  return priv->num_timestamp_updates;
}

/* Return number of calls to emer_persistent_cache_store_metrics() */
gint
mock_persistent_cache_get_store_metrics_called (EmerPersistentCache *self)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  return priv->store_metrics_called;
}
