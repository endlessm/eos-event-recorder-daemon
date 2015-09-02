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

#include <glib.h>

#include "shared/metrics-util.h"

typedef struct _EmerPersistentCachePrivate
{
  GPtrArray *variant_array;
} EmerPersistentCachePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerPersistentCache, emer_persistent_cache,
                            G_TYPE_OBJECT);

static void
emer_persistent_cache_init (EmerPersistentCache *self)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  priv->variant_array =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
}

static void
emer_persistent_cache_finalize (GObject *object)
{
  EmerPersistentCache *self = EMER_PERSISTENT_CACHE (object);
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  g_ptr_array_unref (priv->variant_array);

  G_OBJECT_CLASS (emer_persistent_cache_parent_class)->finalize (object);
}
static void
emer_persistent_cache_class_init (EmerPersistentCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = emer_persistent_cache_finalize;
}

EmerPersistentCache *
emer_persistent_cache_new (const gchar *directory,
                           GError     **error)
{
  return g_object_new (EMER_TYPE_PERSISTENT_CACHE, NULL);
}

EmerPersistentCache *
emer_persistent_cache_new_full (const gchar              *directory,
                                EmerCacheSizeProvider    *cache_size_provider,
                                EmerBootIdProvider       *boot_id_provider,
                                EmerCacheVersionProvider *version_provider,
                                guint                     boot_offset_update_interval,
                                GError                  **error)
{
  g_assert_not_reached ();
}

gsize
emer_persistent_cache_cost (GVariant *variant)
{
  return g_variant_get_size (variant);
}

gboolean
emer_persistent_cache_get_boot_time_offset (EmerPersistentCache *self,
                                            gint64              *offset,
                                            GError             **error)
{
  if (offset != NULL)
    *offset = BOOT_TIME_OFFSET;
  return TRUE;
}

gboolean
emer_persistent_cache_store (EmerPersistentCache *self,
                             GVariant           **variants,
                             gsize                num_variants,
                             gsize               *num_variants_stored,
                             GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  for (gsize i = 0; priv->variant_array->len < MAX_NUM_VARIANTS &&
       i < num_variants; i++)
    {
      g_variant_ref_sink (variants[i]);
      g_ptr_array_add (priv->variant_array, variants[i]);
    }

  *num_variants_stored = num_variants;

  return TRUE;
}

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

  gsize curr_cost = 0, curr_num_variants = 0;
  for (; curr_num_variants < priv->variant_array->len; curr_num_variants++)
    {
      GVariant *curr_variant =
        g_ptr_array_index (priv->variant_array, curr_num_variants);
      curr_cost += emer_persistent_cache_cost (curr_variant);
      if (curr_cost > cost)
        break;
    }

  *num_variants = curr_num_variants;

  *variants = g_new (GVariant *, curr_num_variants);
  for (gsize i = 0; i < curr_num_variants; i++)
    {
      GVariant *curr_variant = g_ptr_array_index (priv->variant_array, i);
      (*variants)[i] = deep_copy_variant (curr_variant);
    }

  *token = curr_num_variants;
  return TRUE;
}

gboolean
emer_persistent_cache_has_more (EmerPersistentCache *self,
                                guint64              token)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  return token < priv->variant_array->len;
}

gboolean
emer_persistent_cache_remove (EmerPersistentCache *self,
                              guint64              token,
                              GError             **error)
{
  EmerPersistentCachePrivate *priv =
    emer_persistent_cache_get_instance_private (self);

  if (token > 0)
    g_ptr_array_remove_range (priv->variant_array, 0, token);

  return TRUE;
}
