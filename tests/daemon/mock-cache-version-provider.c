/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014, 2015 Endless Mobile, Inc. */

#include "emer-cache-version-provider.h"

typedef struct _EmerCacheVersionProviderPrivate
{
  gint cache_version;
} EmerCacheVersionProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerCacheVersionProvider,
                            emer_cache_version_provider,
                            G_TYPE_OBJECT);

static void
emer_cache_version_provider_class_init (EmerCacheVersionProviderClass *klass)
{
  /* Nothing to do */
}

static void
emer_cache_version_provider_init (EmerCacheVersionProvider *self)
{
  /* Nothing to do */
}

EmerCacheVersionProvider *
emer_cache_version_provider_new (const gchar *path)
{
  return g_object_new (EMER_TYPE_CACHE_VERSION_PROVIDER, NULL);
}

gboolean
emer_cache_version_provider_get_version (EmerCacheVersionProvider *self,
                                         gint                     *version)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);

  *version = priv->cache_version;
  return TRUE;
}

gboolean
emer_cache_version_provider_set_version (EmerCacheVersionProvider *self,
                                         gint                      new_version,
                                         GError                  **error)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);

  priv->cache_version = new_version;
  return TRUE;
}
