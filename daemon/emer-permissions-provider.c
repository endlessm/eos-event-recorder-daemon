/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-permissions-provider.h"

#include <glib.h>
#include <gio/gio.h>

typedef struct _EmerPermissionsProviderPrivate
{
  /* Permissions, cached from config file */
  GKeyFile *permissions;

  /* For reading the config file */
  GFile *config_file;
} EmerPermissionsProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerPermissionsProvider, emer_permissions_provider, G_TYPE_OBJECT)

#define DEFAULT_CONFIG_FILE_PATH "/etc/eos-metrics-permissions.conf"

#define DAEMON_ENABLED_GROUP_NAME "global"
#define DAEMON_ENABLED_KEY_NAME "enabled"

#define FALLBACK_CONFIG_FILE_DATA \
  "[" DAEMON_ENABLED_GROUP_NAME "]\n" \
  DAEMON_ENABLED_KEY_NAME "=false\n"

enum
{
  PROP_0,
  PROP_CONFIG_FILE_PATH,
  PROP_DAEMON_ENABLED,
  NPROPS
};

static GParamSpec *emer_permissions_provider_props[NPROPS] = { NULL, };

/* Replaces the in-memory permissions config data with the fallback values */
static void
load_fallback_data (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  if (g_key_file_load_from_data (priv->permissions, FALLBACK_CONFIG_FILE_DATA,
                                 -1, G_KEY_FILE_NONE, NULL))
    return;

  /* Time to panic! Programmer error, fallback config data could not be parsed
  properly. Could probably only happen if the fallback data is malformed. */
  g_assert_not_reached ();
}

/* Read config values from the config file, and if that fails assume the
default. Also emits a property notification. */
static void
read_config_file_sync (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  GError *error = NULL;

  gchar *path = g_file_get_path (priv->config_file);
  gboolean success = g_key_file_load_from_file (priv->permissions, path,
                                                G_KEY_FILE_NONE, &error);
  if (!success)
    {
      load_fallback_data (self);

      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT) &&
          !g_error_matches (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_NOT_FOUND))
        g_critical ("Permissions config file '%s' was invalid or could not be "
                    "read. Loading fallback data. Message: %s.", path,
                    error->message);
      /* but if the config file was simply not there, fail silently and stick
      with the defaults */

      g_clear_error (&error);
    }

  g_object_notify (G_OBJECT (self), "daemon-enabled");

  g_free (path);
}

static void
emer_permissions_provider_constructed (GObject *object)
{
  EmerPermissionsProvider *self = EMER_PERMISSIONS_PROVIDER (object);

  /* One blocking call on daemon startup (usually once per boot) */
  read_config_file_sync (self);

  G_OBJECT_CLASS (emer_permissions_provider_parent_class)->constructed (object);
}

/* Construct-only setter */
static void
set_config_file_path (EmerPermissionsProvider *self,
                      const gchar             *path)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  priv->config_file = g_file_new_for_path (path);
}

static void
emer_permissions_provider_get_property (GObject    *object,
                                        guint       property_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  EmerPermissionsProvider *self = EMER_PERMISSIONS_PROVIDER (object);

  switch (property_id)
    {
    case PROP_DAEMON_ENABLED:
      g_value_set_boolean (value,
                           emer_permissions_provider_get_daemon_enabled (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_permissions_provider_set_property (GObject      *object,
                                        guint         property_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  EmerPermissionsProvider *self = EMER_PERMISSIONS_PROVIDER (object);

  switch (property_id)
    {
    case PROP_CONFIG_FILE_PATH:
      set_config_file_path (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_permissions_provider_finalize (GObject *object)
{
  EmerPermissionsProvider *self = EMER_PERMISSIONS_PROVIDER (object);
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  g_key_file_unref (priv->permissions);
  g_clear_object (&priv->config_file);

  G_OBJECT_CLASS (emer_permissions_provider_parent_class)->finalize (object);
}

static void
emer_permissions_provider_class_init (EmerPermissionsProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = emer_permissions_provider_constructed;
  object_class->get_property = emer_permissions_provider_get_property;
  object_class->set_property = emer_permissions_provider_set_property;
  object_class->finalize = emer_permissions_provider_finalize;

  emer_permissions_provider_props[PROP_CONFIG_FILE_PATH] =
    g_param_spec_string ("config-file-path", "Config file path",
                         "Path to permissions configuration file",
                         DEFAULT_CONFIG_FILE_PATH,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  emer_permissions_provider_props[PROP_DAEMON_ENABLED] =
    g_param_spec_boolean ("daemon-enabled", "Daemon enabled",
                          "Whether to enable the metrics daemon system-wide",
                          FALSE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_permissions_provider_props);
}

static void
emer_permissions_provider_init (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  priv->permissions = g_key_file_new ();
}

/* PUBLIC API */

/*
 * emer_permissions_provider_new:
 *
 * Creates a new permissions provider with the default config file path.
 * This object controls whether the metrics event recorder server should record
 * events or not.
 *
 * Returns: (transfer full): a new permissions provider.
 *   Free with g_object_unref() when done.
 */
EmerPermissionsProvider *
emer_permissions_provider_new (void)
{
  return g_object_new (EMER_TYPE_PERMISSIONS_PROVIDER, NULL);
}

/*
 * emer_permissions_provider_new_full:
 *
 * Creates a new permissions provider with a custom config file path.
 * Use this function only for testing purposes.
 *
 * Returns: (transfer full): a new permissions provider.
 *   Free with g_object_unref() when done.
 */
EmerPermissionsProvider *
emer_permissions_provider_new_full (const char *config_file_path)
{
  return g_object_new (EMER_TYPE_PERMISSIONS_PROVIDER,
                       "config-file-path", config_file_path,
                       NULL);
}

/*
 * emer_permissions_provider_get_daemon_enabled:
 * @self: the permissions provider
 *
 * Tells whether the event recorder should record events.
 *
 * Returns: %TRUE if the event recorder is allowed to record events, %FALSE if
 *   the user has opted out or the user's preference is unknown.
 */
gboolean
emer_permissions_provider_get_daemon_enabled (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  GError *error = NULL;
  gboolean retval = g_key_file_get_boolean (priv->permissions,
                                            DAEMON_ENABLED_GROUP_NAME,
                                            DAEMON_ENABLED_KEY_NAME, &error);
  if (error != NULL)
    {
      g_critical ("Couldn't find key '%s:%s' in permissions config file. "
                  "Returning default value. Message: %s.",
                  DAEMON_ENABLED_GROUP_NAME, DAEMON_ENABLED_KEY_NAME,
                  error->message);
      /* retval is FALSE in case of error */
    }

  return retval;
}
