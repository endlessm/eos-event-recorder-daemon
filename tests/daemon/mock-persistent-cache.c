/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-persistent-cache.h"
#include "mock-persistent-cache.h"

typedef struct _EmerPersistentCachePrivate
{
  gboolean foo;
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
                                gchar                    *custom_directory,
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
  *num_singulars_stored = num_singulars_buffered;
  *num_aggregates_stored = num_aggregates_buffered;
  *num_sequences_stored = num_sequences_buffered;
  *capacity = CAPACITY_LOW;
  return TRUE;
}
