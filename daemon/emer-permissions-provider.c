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

#include "emer-permissions-provider.h"

#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <ostree-repo.h>

#include "shared/metrics-util.h"

typedef struct _EmerPermissionsProviderPrivate
{
  /* Permissions, cached from config file */
  GKeyFile *permissions;

  /* For reading the config file */
  GFile *permissions_config_file;

  /* Source ID for write_config_file_idle_cb */
  guint write_config_file_idle_id;

  /* For reading the OSTree config file */
  GFile *ostree_config_file;
} EmerPermissionsProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerPermissionsProvider, emer_permissions_provider, G_TYPE_OBJECT)

#define DAEMON_GLOBAL_GROUP_NAME "global"
#define DAEMON_ENABLED_KEY_NAME "enabled"
#define DAEMON_UPLOADING_ENABLED_KEY_NAME "uploading_enabled"
#define DAEMON_ENVIRONMENT_KEY_NAME "environment"

#define FALLBACK_CONFIG_FILE_DATA \
  "[" DAEMON_GLOBAL_GROUP_NAME "]\n" \
  DAEMON_ENABLED_KEY_NAME "=true\n" \
  DAEMON_UPLOADING_ENABLED_KEY_NAME "=false\n" \
  DAEMON_ENVIRONMENT_KEY_NAME "=production\n"

enum
{
  PROP_0,
  PROP_PERMISSIONS_CONFIG_FILE_PATH,
  PROP_OSTREE_CONFIG_FILE_PATH,
  PROP_DAEMON_ENABLED,
  PROP_UPLOADING_ENABLED,
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

/* Writes current in-memory data to the config file */
static void
write_config_file_sync (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  gchar *permissions_config_file_path =
    g_file_get_path (priv->permissions_config_file);
  GError *error = NULL;

  if (!g_key_file_save_to_file (priv->permissions, permissions_config_file_path,
      &error))
    {
      g_critical ("Could not write to permissions config file '%s'. Error: %s.",
                  permissions_config_file_path, error->message);
      g_clear_error (&error);
    }

  g_free (permissions_config_file_path);
}

/* Read config values from the config file, and if that fails assume the
default. Also emits a property notification. */
static void
read_config_file_sync (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  GError *error = NULL;

  gchar *path = g_file_get_path (priv->permissions_config_file);
  gboolean load_succeeded =
    g_key_file_load_from_file (priv->permissions, path, G_KEY_FILE_NONE, &error);
  if (!load_succeeded)
    {
      load_fallback_data (self);

      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_critical ("Permissions config file '%s' was invalid or could not be "
                    "read. Loading fallback data. Error: %s.", path,
                    error->message);
      /* If the config file was simply not there, fail silently and stick
       * with the defaults. */

      g_clear_error (&error);
    }

  g_free (path);

  GParamSpec *daemon_enabled_pspec =
    emer_permissions_provider_props[PROP_DAEMON_ENABLED];
  g_object_notify_by_pspec (G_OBJECT (self), daemon_enabled_pspec);

  GParamSpec *uploading_enabled_pspec =
    emer_permissions_provider_props[PROP_UPLOADING_ENABLED];
  g_object_notify_by_pspec (G_OBJECT (self), uploading_enabled_pspec);

}

static gboolean
write_config_file_idle_cb (gpointer data)
{
  EmerPermissionsProvider *self = EMER_PERMISSIONS_PROVIDER (data);
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  write_config_file_sync (self);
  priv->write_config_file_idle_id = 0;

  return G_SOURCE_REMOVE;
}

/* Schedule a call to write_config_file_sync(). Don't wait for it to finish. */
static void
schedule_config_file_update (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  if (priv->write_config_file_idle_id != 0)
    return;

  priv->write_config_file_idle_id =
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                     write_config_file_idle_cb,
                     g_object_ref (self),
                     g_object_unref);
}

static gchar *
get_ostree_url_from_file (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  gchar *path = g_file_get_path (priv->ostree_config_file);

  if (path == NULL)
    {
      g_warning ("Unable to get file path from given OSTree config file.");
      return NULL;
    }

  GKeyFile *ostree_configuration_key_file = g_key_file_new ();
  GError *error = NULL;
  if (!g_key_file_load_from_file (ostree_configuration_key_file, path,
                                 G_KEY_FILE_NONE, &error))
    {
      g_warning ("Unable to load OSTree GKeyFile from given OSTree config "
                 "file path %s. Error: %s.", path, error->message);
      g_free (path);
      g_clear_error (&error);
      return NULL;
    }

  g_free (path);

  gchar *ostree_url = g_key_file_get_value (ostree_configuration_key_file,
                                            "remote \"eos\"",
                                            "url",
                                            &error);
  g_key_file_unref (ostree_configuration_key_file);

  if (ostree_url == NULL)
    {
      g_warning ("Unable to read OSTree URL from given OSTree config file. "
                 "Error: %s.", error->message);
      g_clear_error (&error);
    }

  return ostree_url;
}

static gchar *
get_ostree_url_from_ostree_repo (void)
{
  OstreeRepo *ostree_repo = ostree_repo_new_default ();

  GError *error = NULL;
  if (!ostree_repo_open (ostree_repo, NULL /* GCancellable */, &error))
    {
      g_warning ("Unable to open OSTree repo. Error: %s.", error->message);
      g_clear_error (&error);
      g_object_unref (ostree_repo);
      return NULL;
    }

  GKeyFile *ostree_configuration_file = ostree_repo_get_config (ostree_repo);

  if (ostree_configuration_file == NULL)
    {
      g_warning ("Unable to load OSTree configuration file.");
      g_object_unref (ostree_repo);
      return NULL;
    }

  gchar *ostree_url = g_key_file_get_value (ostree_configuration_file,
                                            "remote \"eos\"",
                                            "url",
                                            &error);
  g_object_unref (ostree_repo);

  if (ostree_url == NULL)
    {
      g_warning ("Unable to read OSTree URL. Error: %s.", error->message);
      g_clear_error (&error);
    }

  return ostree_url;
}

static gchar *
read_ostree_url (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  if (priv->ostree_config_file == NULL)
    return get_ostree_url_from_ostree_repo ();
  else
    return get_ostree_url_from_file (self);
}

static gchar *
read_environment (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  GError *error = NULL;
  gchar *environment = g_key_file_get_value (priv->permissions,
                                             DAEMON_GLOBAL_GROUP_NAME,
                                             DAEMON_ENVIRONMENT_KEY_NAME,
                                             &error);
  if (error != NULL)
    {
      g_critical ("Couldn't find key '%s:%s' in permissions config file. "
                  "Returning default value. Error: %s.",
                  DAEMON_GLOBAL_GROUP_NAME, DAEMON_ENVIRONMENT_KEY_NAME,
                  error->message);
      g_error_free (error);
    }

  if (g_strcmp0 (environment, "dev") != 0 &&
      g_strcmp0 (environment, "test") != 0 &&
      g_strcmp0 (environment, "production") != 0)
    {
      g_warning ("Error: Metrics environment is set to: %s in %s. Valid "
                 "metrics environments are: dev, test, production.",
                 environment, PERMISSIONS_FILE);
      g_clear_pointer (&environment, g_free);
    }

  if (environment == NULL)
    {
      g_warning ("Metrics environment was not present or was invalid. Assuming "
                 "'test' environment.");
      return g_strdup ("test");
    }

  return environment;
}

static void
set_environment (EmerPermissionsProvider *self,
                 const gchar             *environment)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  g_key_file_set_string (priv->permissions, DAEMON_GLOBAL_GROUP_NAME,
                         DAEMON_ENVIRONMENT_KEY_NAME, environment);

  schedule_config_file_update (self);
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

  priv->permissions_config_file = g_file_new_for_path (path);
}

/* Construct-only setter */
static void
set_ostree_config_file_path (EmerPermissionsProvider *self,
                             const gchar             *path)
{
  if (path == NULL)
    return;

  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  priv->ostree_config_file = g_file_new_for_path (path);
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

    case PROP_UPLOADING_ENABLED:
      g_value_set_boolean (value,
                           emer_permissions_provider_get_uploading_enabled (self));
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
    case PROP_PERMISSIONS_CONFIG_FILE_PATH:
      set_config_file_path (self, g_value_get_string (value));
      break;

    case PROP_OSTREE_CONFIG_FILE_PATH:
      set_ostree_config_file_path (self, g_value_get_string (value));
      break;

    case PROP_DAEMON_ENABLED:
      emer_permissions_provider_set_daemon_enabled (self,
                                                    g_value_get_boolean (value));
      break;

    case PROP_UPLOADING_ENABLED:
      emer_permissions_provider_set_uploading_enabled (self,
                                                       g_value_get_boolean (value));
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
  g_clear_object (&priv->permissions_config_file);
  g_clear_object (&priv->ostree_config_file);

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

  emer_permissions_provider_props[PROP_PERMISSIONS_CONFIG_FILE_PATH] =
    g_param_spec_string ("config-file-path", "Config file path",
                         "Path to permissions configuration file",
                         PERMISSIONS_FILE,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  emer_permissions_provider_props[PROP_OSTREE_CONFIG_FILE_PATH] =
    g_param_spec_string ("ostree-config-file-path", "OSTree config file path",
                         "Path to OSTree configuration file",
                         NULL,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  emer_permissions_provider_props[PROP_DAEMON_ENABLED] =
    g_param_spec_boolean ("daemon-enabled", "Daemon enabled",
                          "Whether to enable the metrics daemon system-wide",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  emer_permissions_provider_props[PROP_UPLOADING_ENABLED] =
    g_param_spec_boolean ("uploading-enabled", "Uploading enabled",
                          "Whether to upload events via the network",
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_permissions_provider_props);
}

static void
emer_permissions_provider_init (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  priv->permissions = g_key_file_new ();
  priv->ostree_config_file = NULL;
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
 * Free with g_object_unref() when done.
 */
EmerPermissionsProvider *
emer_permissions_provider_new (void)
{
  return g_object_new (EMER_TYPE_PERMISSIONS_PROVIDER, NULL);
}

/*
 * emer_permissions_provider_new_full:
 *
 * Creates a new permissions provider with a custom config file path and custom
 * OSTree config file path. Use this function only for testing purposes.
 *
 * Returns: (transfer full): a new permissions provider.
 * Free with g_object_unref() when done.
 */
EmerPermissionsProvider *
emer_permissions_provider_new_full (const gchar *permissions_config_file_path,
                                    const gchar *ostree_config_file_path)
{
  return g_object_new (EMER_TYPE_PERMISSIONS_PROVIDER,
                       "config-file-path", permissions_config_file_path,
                       "ostree-config-file-path", ostree_config_file_path,
                       NULL);
}

/*
 * emer_permissions_provider_get_daemon_enabled:
 * @self: the permissions provider
 *
 * Tells whether the event recorder should record events.
 *
 * Returns: %TRUE if the event recorder is allowed to record events, %FALSE if
 * the user has opted out or the user's preference is unknown.
 */
gboolean
emer_permissions_provider_get_daemon_enabled (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  GError *error = NULL;
  gboolean daemon_enabled =
    g_key_file_get_boolean (priv->permissions, DAEMON_GLOBAL_GROUP_NAME,
                            DAEMON_ENABLED_KEY_NAME, &error);
  if (error != NULL)
    {
      g_critical ("Couldn't find key '%s:%s' in permissions config file. "
                  "Returning default value. Error: %s.",
                  DAEMON_GLOBAL_GROUP_NAME, DAEMON_ENABLED_KEY_NAME,
                  error->message);
      g_error_free (error);
    }

  return daemon_enabled;
}

/*
 * emer_permissions_provider_set_daemon_enabled:
 * @self: the permissions provider
 * @enabled: whether recording metrics is allowed
 *
 * Sets whether the event recorder should record events.
 */
void
emer_permissions_provider_set_daemon_enabled (EmerPermissionsProvider *self,
                                              gboolean                 enabled)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  g_key_file_set_boolean (priv->permissions, DAEMON_GLOBAL_GROUP_NAME,
                          DAEMON_ENABLED_KEY_NAME, enabled);

  schedule_config_file_update (self);

  GParamSpec *daemon_enabled_pspec =
    emer_permissions_provider_props[PROP_DAEMON_ENABLED];
  g_object_notify_by_pspec (G_OBJECT (self), daemon_enabled_pspec);
}

/*
 * emer_permissions_provider_get_uploading_enabled:
 * @self: the permissions provider
 *
 * Tells whether the event recorder should upload events via the network. This
 * setting is moot if the entire daemon is disabled; see
 * emer_permissions_provider_get_daemon_enabled().
 *
 * Returns: %TRUE if the event recorder is allowed to upload events or the
 * user's preference is unknown, %FALSE if the user has opted out.
 */
gboolean
emer_permissions_provider_get_uploading_enabled (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  GError *error = NULL;
  gboolean uploading_enabled =
    g_key_file_get_boolean (priv->permissions, DAEMON_GLOBAL_GROUP_NAME,
                            DAEMON_UPLOADING_ENABLED_KEY_NAME, &error);
  if (error != NULL)
    {
      g_critical ("Couldn't find key '%s:%s' in permissions config file. "
                  "Returning default value. Error: %s.",
                  DAEMON_GLOBAL_GROUP_NAME, DAEMON_UPLOADING_ENABLED_KEY_NAME,
                  error->message);
      g_error_free (error);
      return TRUE;
    }

  return uploading_enabled;
}

/*
 * emer_permissions_provider_set_uploading_enabled:
 * @self: the permissions provider
 * @enabled: whether the event recorder should upload events via the network
 *
 * Sets whether the event recorder should upload events via the network. This
 * setting is moot if the entire daemon is disabled; see
 * emer_permissions_provider_set_daemon_enabled().
 */
void
emer_permissions_provider_set_uploading_enabled (EmerPermissionsProvider *self,
                                                 gboolean                 enabled)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  g_key_file_set_boolean (priv->permissions, DAEMON_GLOBAL_GROUP_NAME,
                          DAEMON_UPLOADING_ENABLED_KEY_NAME, enabled);

  schedule_config_file_update (self);

  GParamSpec *uploading_enabled_pspec =
    emer_permissions_provider_props[PROP_UPLOADING_ENABLED];
  g_object_notify_by_pspec (G_OBJECT (self), uploading_enabled_pspec);

}

/*
 * emer_permissions_provider_get_environment:
 * @self: the permissions provider
 *
 * Reads the current metrics environment off the disk.
 *
 * Returns: the metrics environment string if it exists in the permissions file
 * and is valid. The default value of the environment string is "test".
 */
gchar *
emer_permissions_provider_get_environment (EmerPermissionsProvider *self)
{
  /* Update the cached permissions file. */
  read_config_file_sync (self);

  gchar *environment = read_environment (self);
  gchar *ostree_url = read_ostree_url (self);

  if (ostree_url == NULL)
    {
      return environment;
    }

  /* Check if the environment is set to "production" and if the term "staging"
   * is in the OSTree URL, which indicates that the metrics environment should
   * be set to "dev".
   */
  if (g_strcmp0 (environment, "production") == 0 &&
      strstr (ostree_url, "staging") != NULL)
    {
      g_clear_pointer (&environment, g_free);
      environment = g_strdup ("dev");

      set_environment (self, environment);
    }

  g_clear_pointer (&ostree_url, g_free);
  return environment;
}
