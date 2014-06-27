/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-cache-version-provider.h"

#include <stdio.h>

typedef struct EmerCacheVersionProviderPrivate
{
  gchar *path;
  gint version;
  gboolean version_cached;
  GKeyFile *key_file;
} EmerCacheVersionProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerCacheVersionProvider, emer_cache_version_provider, G_TYPE_OBJECT)

#define CACHE_VERSION_PROVIDER_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), EMER_TYPE_CACHE_VERSION_PROVIDER, EmerCacheVersionProviderPrivate))

/*
 * The filepath to the meta file containing the local network protocol version.
 */
#define DEFAULT_CACHE_VERSION_FILE_PATH "/var/cache/metrics/local_version_file"

#define CACHE_VERSION_GROUP "cache_version_info"
#define CACHE_VERSION_KEY   "version"

enum {
  PROP_0,
  PROP_PATH,
  NPROPS
};

static GParamSpec *emer_cache_version_provider_props[NPROPS] = { NULL, };

/*
 * SECTION:emer-cache-version-provider
 * @title: Cache Version Provider
 * @short_description: Provides the local cache network protocol version.
 * @include: eosmetrics/eosmetrics.h
 *
 * The version provider supplies a version number which identifies the current
 * network protocol this system's persistent cache is configured to deliver and
 * metrics via.
 *
 * This class abstracts away how and where this version number is generated
 * by providing a simple interface via emer_version_provider_get_version() to
 * whatever calling code needs it.
 */

static const gchar *
get_cache_version_path (EmerCacheVersionProvider *self)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);
  return priv->path;
}

static void
set_cache_version_path (EmerCacheVersionProvider *self,
                        const gchar              *given_path)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);
  priv->path = g_strdup (given_path);
}

static void
emer_cache_version_provider_get_property (GObject    *object,
                                          guint       property_id,
                                          GValue     *value,
                                          GParamSpec *pspec)
{
  EmerCacheVersionProvider *self = EMER_CACHE_VERSION_PROVIDER (object);

  switch (property_id)
    {
    case PROP_PATH:
      g_value_set_string (value, get_cache_version_path (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
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
                         DEFAULT_CACHE_VERSION_FILE_PATH,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  object_class->get_property = emer_cache_version_provider_get_property;
  object_class->set_property = emer_cache_version_provider_set_property;
  object_class->finalize = emer_cache_version_provider_finalize;

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_cache_version_provider_props);
}

// Mandatory empty function.
static void
emer_cache_version_provider_init (EmerCacheVersionProvider *self)
{
  EmerCacheVersionProviderPrivate *priv =
    emer_cache_version_provider_get_instance_private (self);
  priv->key_file = g_key_file_new ();
}

/*
 * emer_cache_version_provider_new:
 *
 * Constructs the ID provider used to obtain a cache network protocol version
 * via the default filepath.
 *
 * Returns: (transfer full): A new #EmerCacheVersionProvider.
 * Free with g_object_unref().
 */
EmerCacheVersionProvider *
emer_cache_version_provider_new (void)
{
  return g_object_new (EMER_TYPE_CACHE_VERSION_PROVIDER,
                       "path", DEFAULT_CACHE_VERSION_FILE_PATH,
                       NULL);
}

/*
 * emer_cache_version_provider_new_full:
 * @cache_version_file_path: path to a file; see #EmerCacheVersionProvider:path
 *
 * Constructs the ID provider used to obtain a cache network protocol version
 * via a given filepath.
 *
 * Returns: (transfer full): A new #EmerCacheVersionProvider.
 * Free with g_object_unref().
 */
EmerCacheVersionProvider *
emer_cache_version_provider_new_full (const gchar *cache_version_file_path)
{
  return g_object_new (EMER_TYPE_CACHE_VERSION_PROVIDER,
                       "path", cache_version_file_path,
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
      g_critical ("Failed to read cache version! Error: %s.", error->message);
      g_error_free (error);
      return FALSE;
    }

  priv->version = g_key_file_get_integer (priv->key_file, CACHE_VERSION_GROUP,
                                          CACHE_VERSION_KEY, &error);

  if (error != NULL)
    {
      g_critical ("Failed to read cache version! Error: %s.", error->message);
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
 * Retrieves the cache protocol version number.
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
    }

  *version = priv->version;
  priv->version_cached = TRUE;
  return TRUE;
}

/*
 * Updates the cache version file number and creates a new meta_file if
 * one doesn't exist. Returns %TRUE on success, and %FALSE on failure.
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
      g_prefix_error (error, "Failed to write to version file: %s .",
                      priv->path);
      return FALSE;
    }

  priv->version = new_version;
  priv->version_cached = TRUE;
  return TRUE;
}
