/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2015 Endless Mobile, Inc. */

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

#include "emer-cache-size-provider.h"

typedef struct EmerCacheSizeProviderPrivate
{
  gchar *path;
  guint64 max_cache_size;
  gboolean data_cached;
  GKeyFile *key_file;
} EmerCacheSizeProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerCacheSizeProvider, emer_cache_size_provider, G_TYPE_OBJECT)

/*
 * The filepath to the metadata file containing the maximum persistent cache
 * size.
 */
#define DEFAULT_CACHE_SIZE_FILE_PATH CONFIG_DIR "cache-size.conf"

/* This is the default maximum cache size in bytes. */
#define DEFAULT_MAX_CACHE_SIZE G_GUINT64_CONSTANT (10000000)

#define CACHE_SIZE_GROUP "persistent_cache_size"
#define MAX_CACHE_SIZE_KEY "maximum"

enum
{
  PROP_0,
  PROP_PATH,
  NPROPS
};

static GParamSpec *emer_cache_size_provider_props[NPROPS] = { NULL, };

/*
 * SECTION:emer-cache-size-provider
 * @title: Cache Size Provider
 * @short_description: Specifies the maximum permissable size of the persistent
 * cache.
 *
 * Abstracts away how the maximum cache size is specified via the
 * emer_cache_size_provider_get_max_cache_size method.
 */

static void
set_cache_size_path (EmerCacheSizeProvider *self,
                     const gchar           *given_path)
{
  EmerCacheSizeProviderPrivate *priv =
    emer_cache_size_provider_get_instance_private (self);
  priv->path = g_strdup (given_path);
}

static void
emer_cache_size_provider_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  EmerCacheSizeProvider *self = EMER_CACHE_SIZE_PROVIDER (object);

  switch (property_id)
    {
    case PROP_PATH:
      set_cache_size_path (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
emer_cache_size_provider_finalize (GObject *object)
{
  EmerCacheSizeProvider *self = EMER_CACHE_SIZE_PROVIDER (object);
  EmerCacheSizeProviderPrivate *priv =
    emer_cache_size_provider_get_instance_private (self);

  g_free (priv->path);
  g_key_file_unref (priv->key_file);
  G_OBJECT_CLASS (emer_cache_size_provider_parent_class)->finalize (object);
}

static void
emer_cache_size_provider_class_init (EmerCacheSizeProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  /* Blurb string is good enough default documentation for this. */
  emer_cache_size_provider_props[PROP_PATH] =
    g_param_spec_string ("path", "Path",
                         "The path to the file where the maximum persistent "
                         "cache size is stored.",
                         DEFAULT_CACHE_SIZE_FILE_PATH,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  object_class->set_property = emer_cache_size_provider_set_property;
  object_class->finalize = emer_cache_size_provider_finalize;

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_cache_size_provider_props);
}

static void
emer_cache_size_provider_init (EmerCacheSizeProvider *self)
{
  EmerCacheSizeProviderPrivate *priv =
    emer_cache_size_provider_get_instance_private (self);
  priv->key_file = g_key_file_new ();
}

/*
 * emer_cache_size_provider_new:
 *
 * Constructs a provider that obtains the maximum persistent cache size from the
 * default filepath.
 *
 * Returns: (transfer full): A new #EmerCacheSizeProvider.
 * Free with g_object_unref().
 */
EmerCacheSizeProvider *
emer_cache_size_provider_new (void)
{
  return g_object_new (EMER_TYPE_CACHE_SIZE_PROVIDER, NULL);
}

/*
 * emer_cache_size_provider_new_full:
 * @path: path to a file specifying a maximum persistent cache size; see
 * #EmerCacheSizeProvider:path.
 *
 * Constructs a provider that obtains the maximum persistent cache size from the
 * given filepath.
 *
 * Returns: (transfer full): A new #EmerCacheSizeProvider.
 * Free with g_object_unref().
 */
EmerCacheSizeProvider *
emer_cache_size_provider_new_full (const gchar *path)
{
  return g_object_new (EMER_TYPE_CACHE_SIZE_PROVIDER,
                       "path", path,
                       NULL);
}

static void
write_cache_size (EmerCacheSizeProvider *self,
                  guint64                max_cache_size)
{
  EmerCacheSizeProviderPrivate *priv =
    emer_cache_size_provider_get_instance_private (self);

  g_key_file_set_uint64 (priv->key_file, CACHE_SIZE_GROUP,
                         MAX_CACHE_SIZE_KEY, max_cache_size);

  GError *error = NULL;
  gboolean save_succeeded =
    g_key_file_save_to_file (priv->key_file, priv->path, &error);
  if (!save_succeeded)
    {
      g_warning ("Failed to write default cache size file to %s. Error: %s.",
                 priv->path, error->message);
      g_error_free (error);
    }
}

static void
read_cache_size_data (EmerCacheSizeProvider *self)
{
  EmerCacheSizeProviderPrivate *priv =
    emer_cache_size_provider_get_instance_private (self);

  if (priv->data_cached)
    return;

  GError *error = NULL;
  if (!g_key_file_load_from_file (priv->key_file, priv->path, G_KEY_FILE_NONE,
                                  &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        goto handle_failed_read;

      g_clear_error (&error);
      write_cache_size (self, DEFAULT_MAX_CACHE_SIZE);
    }

  priv->max_cache_size = g_key_file_get_uint64 (priv->key_file,
                                                CACHE_SIZE_GROUP,
                                                MAX_CACHE_SIZE_KEY,
                                                &error);
  if (error != NULL)
    goto handle_failed_read;

  priv->data_cached = TRUE;
  return;

handle_failed_read:
  g_warning ("Failed to read from cache size file. Error: %s.", error->message);
  g_error_free (error);
}

/*
 * emer_cache_size_provider_get_max_cache_size:
 * @self: the max cache size provider
 *
 * Returns the maximum persistent cache size in bytes. If the underlying
 * configuration file doesn't exist yet, it is created with a default value of
 * DEFAULT_MAX_CACHE_SIZE.
 */
guint64
emer_cache_size_provider_get_max_cache_size (EmerCacheSizeProvider *self)
{
  EmerCacheSizeProviderPrivate *priv =
    emer_cache_size_provider_get_instance_private (self);

  read_cache_size_data (self);
  return priv->max_cache_size;
}
