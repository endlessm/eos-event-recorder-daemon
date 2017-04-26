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

#include "emer-cache-version-provider.h"

typedef struct EmerCacheVersionProviderPrivate
{
  gchar *path;
  gint version;
  gboolean version_cached;
  GKeyFile *key_file;
} EmerCacheVersionProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerCacheVersionProvider, emer_cache_version_provider, G_TYPE_OBJECT)

#define CACHE_VERSION_GROUP "cache_version_info"
#define CACHE_VERSION_KEY   "version"

enum
{
  PROP_0,
  PROP_PATH,
  NPROPS
};

static GParamSpec *emer_cache_version_provider_props[NPROPS] = { NULL, };

/*
 * SECTION:emer-cache-version-provider
 * @title: Cache Version Provider
 * @short_description: Provides the local cache format version.
 *
 * The version provider supplies a version number which identifies the current
 * format this system's persistent cache is configured to store and
 * return metrics in. Existing metrics in the persistent cache will be
 * consistent with this format as all metrics in the cache are purged when the
 * version changes.
 *
 * This class abstracts away how and where this version number is generated and
 * stored by providing a simple interface via
 * emer_version_provider_get_version() and
 * emer_version_provider_set_version() to whatever calling code needs it.
 */

static void
set_cache_version_path (EmerCacheVersionProvider *self,
                        const gchar              *given_path)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);
  priv->path = g_strdup (given_path);
}

static void
emer_cache_version_provider_set_property (GObject      *object,
                                          guint         property_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  EmerCacheVersionProvider *self = EMER_CACHE_VERSION_PROVIDER (object);

  switch (property_id)
    {
    case PROP_PATH:
      set_cache_version_path (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_cache_version_provider_finalize (GObject *object)
{
  EmerCacheVersionProvider *self = EMER_CACHE_VERSION_PROVIDER (object);
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);

  g_free (priv->path);
  g_key_file_unref (priv->key_file);

  G_OBJECT_CLASS (emer_cache_version_provider_parent_class)->finalize (object);
}

static void
emer_cache_version_provider_class_init (EmerCacheVersionProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  /* Blurb string is good enough default documentation for this. */
  emer_cache_version_provider_props[PROP_PATH] =
    g_param_spec_string ("path", "Path",
                         "The path to the file where the cache version is stored.",
                         NULL,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  object_class->set_property = emer_cache_version_provider_set_property;
  object_class->finalize = emer_cache_version_provider_finalize;

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_cache_version_provider_props);
}

static void
emer_cache_version_provider_init (EmerCacheVersionProvider *self)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);
  priv->key_file = g_key_file_new ();
}

/*
 * emer_cache_version_provider_new:
 * @path: see #EmerCacheVersionProvider:path
 *
 * Constructs a provider that stores the cache format version in a file at the
 * given path.
 *
 * Returns: (transfer full): A new #EmerCacheVersionProvider.
 * Free with g_object_unref().
 */
EmerCacheVersionProvider *
emer_cache_version_provider_new (const gchar *path)
{
  return g_object_new (EMER_TYPE_CACHE_VERSION_PROVIDER,
                       "path", path,
                       NULL);
}

static gboolean
read_cache_version (EmerCacheVersionProvider *self)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);
  GError *error = NULL;
  if (!g_key_file_load_from_file (priv->key_file, priv->path, G_KEY_FILE_NONE,
                                  &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_warning ("Failed to read cache version. Error: %s.", error->message);
        }

      g_error_free (error);
      return FALSE;
    }

  priv->version = g_key_file_get_integer (priv->key_file, CACHE_VERSION_GROUP,
                                          CACHE_VERSION_KEY, &error);

  if (error != NULL)
    {
      g_warning ("Failed to read cache version. Error: %s.", error->message);
      g_error_free (error);
      return FALSE;
    }
  return TRUE;
}

/*
 * emer_cache_version_provider_get_version:
 * @self: the cache version provider.
 * @version: the address of the gint to store the version in.
 *
 * Retrieves the cache format version number.
 *
 * Returns: a boolean indicating success or failure of retrieval.
 * If this returns %FALSE, version cannot be trusted to be valid.
 */
gboolean
emer_cache_version_provider_get_version (EmerCacheVersionProvider *self,
                                         gint                     *version)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);

  if (!priv->version_cached)
    {
      if (!read_cache_version (self))
        return FALSE;
      priv->version_cached = TRUE;
    }

  *version = priv->version;
  return TRUE;
}

/*
 * Updates the cache version number and creates a new metadata file if
 * one doesn't already exist. Returns %TRUE on success and %FALSE on failure.
 */
gboolean
emer_cache_version_provider_set_version (EmerCacheVersionProvider *self,
                                         gint                      new_version,
                                         GError                  **error)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);

  g_key_file_set_integer (priv->key_file, CACHE_VERSION_GROUP,
                          CACHE_VERSION_KEY, new_version);

  if (!g_key_file_save_to_file (priv->key_file, priv->path, error))
    {
      g_prefix_error (error, "Failed to write to version file: %s. ",
                      priv->path);
      return FALSE;
    }

  priv->version = new_version;
  priv->version_cached = TRUE;
  return TRUE;
}
