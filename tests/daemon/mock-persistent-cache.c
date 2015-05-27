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
#include "shared/metrics-util.h"

#include <glib.h>

typedef struct _EmerPersistentCachePrivate
{
  GArray *singular_buffer;
  GArray *aggregate_buffer;
  GArray *sequence_buffer;
  gint num_timestamp_updates;
  gint store_metrics_called;
} EmerPersistentCachePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerPersistentCache, emer_persistent_cache,
                            G_TYPE_OBJECT);

static void
emer_persistent_cache_init (EmerPersistentCache *self)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->singular_buffer = g_array_new (TRUE, FALSE, sizeof (GVariant *));
  g_array_set_clear_func (priv->singular_buffer,
                          (GDestroyNotify) g_variant_unref);

  priv->aggregate_buffer = g_array_new (TRUE, FALSE, sizeof (GVariant *));
  g_array_set_clear_func (priv->aggregate_buffer,
                          (GDestroyNotify) g_variant_unref);

  priv->sequence_buffer = g_array_new (TRUE, FALSE, sizeof (GVariant *));
  g_array_set_clear_func (priv->sequence_buffer,
                          (GDestroyNotify) g_variant_unref);
}

static void
emer_persistent_cache_finalize (GObject *object)
{
  EmerPersistentCache *self = EMER_PERSISTENT_CACHE (object);
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  g_array_unref (priv->singular_buffer);
  g_array_unref (priv->aggregate_buffer);
  g_array_unref (priv->sequence_buffer);

  G_OBJECT_CLASS (emer_persistent_cache_parent_class)->finalize (object);
}
static void
emer_persistent_cache_class_init (EmerPersistentCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = emer_persistent_cache_finalize;
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
                                EmerCacheSizeProvider    *cache_size_provider,
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

  if (offset != NULL)
    *offset = BOOT_TIME_OFFSET;
  return TRUE;
}

static void
drain_to_c_array (GArray    **variant_array,
                  GVariant ***variant_c_array)
{
  *variant_c_array = (GVariant **) g_array_free (*variant_array, FALSE);
  *variant_array = g_array_new (TRUE, FALSE, sizeof (GVariant *));
}

// TODO: Support max_num_bytes parameter once it's supported in production.
gboolean
emer_persistent_cache_drain_metrics (EmerPersistentCache *self,
                                     GVariant          ***singular_array,
                                     GVariant          ***aggregate_array,
                                     GVariant          ***sequence_array,
                                     gint                 max_num_bytes)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  drain_to_c_array (&priv->singular_buffer, singular_array);
  drain_to_c_array (&priv->aggregate_buffer, aggregate_array);
  drain_to_c_array (&priv->sequence_buffer, sequence_array);

  return TRUE;
}

static void
append_singulars (GArray        *singular_variants,
                  SingularEvent *singular_structs,
                  gint           num_singulars_buffered)
{
  for (gint i = 0; i < num_singulars_buffered; i++)
    {
      GVariant *singular = singular_to_variant (singular_structs + i);
      g_array_append_val (singular_variants, singular);
    }
}

static void
append_aggregates (GArray         *aggregate_variants,
                   AggregateEvent *aggregate_structs,
                   gint            num_aggregates_buffered)
{
  for (gint i = 0; i < num_aggregates_buffered; i++)
    {
      GVariant *aggregate = aggregate_to_variant (aggregate_structs + i);
      g_array_append_val (aggregate_variants, aggregate);
    }
}

static void
append_sequences (GArray        *sequence_variants,
                  SequenceEvent *sequence_structs,
                  gint           num_sequences_buffered)
{
  for (gint i = 0; i < num_sequences_buffered; i++)
    {
      GVariant *sequence = sequence_to_variant (sequence_structs + i);
      g_array_append_val (sequence_variants, sequence);
    }
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

  append_singulars (priv->singular_buffer, singular_buffer,
                    num_singulars_buffered);
  *num_singulars_stored = num_singulars_buffered;

  append_aggregates (priv->aggregate_buffer, aggregate_buffer,
                     num_aggregates_buffered);
  *num_aggregates_stored = num_aggregates_buffered;

  append_sequences (priv->sequence_buffer, sequence_buffer,
                    num_sequences_buffered);
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
