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
#include "mock-permissions-provider.h"

#include <glib.h>
#include <gio/gio.h>

typedef struct _EmerPermissionsProviderPrivate
{
  gboolean mock_daemon_enabled;

  gint get_daemon_enabled_called;
} EmerPermissionsProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerPermissionsProvider, emer_permissions_provider, G_TYPE_OBJECT)

static void
emer_permissions_provider_class_init (EmerPermissionsProviderClass *klass)
{
}

static void
emer_permissions_provider_init (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  priv->mock_daemon_enabled = TRUE;
}

/* MOCK PUBLIC API */

EmerPermissionsProvider *
emer_permissions_provider_new (void)
{
  return g_object_new (EMER_TYPE_PERMISSIONS_PROVIDER, NULL);
}

EmerPermissionsProvider *
emer_permissions_provider_new_full (const char *config_file_path,
                                    const char *ostree_config_file_path)
{
  return emer_permissions_provider_new ();
}

gboolean
emer_permissions_provider_get_daemon_enabled (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  priv->get_daemon_enabled_called++;
  return priv->mock_daemon_enabled;
}

void
emer_permissions_provider_set_daemon_enabled (EmerPermissionsProvider *self,
                                              gboolean                 enabled)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  priv->mock_daemon_enabled = enabled;

  /* This works for faking a property notification even though there isn't a
  property by that name in this mock object */
  g_signal_emit_by_name (self, "notify::daemon-enabled", NULL);
}

gchar *
emer_permissions_provider_get_environment (EmerPermissionsProvider *self)
{
  return g_strdup ("test");
}

/* API OF MOCK OBJECT */

/* Return number of calls to emer_permissions_provider_get_daemon_enabled() */
gint
mock_permissions_provider_get_daemon_enabled_called (EmerPermissionsProvider *self)
{
  EmerPermissionsProviderPrivate *priv =
    emer_permissions_provider_get_instance_private (self);

  return priv->get_daemon_enabled_called;
}
