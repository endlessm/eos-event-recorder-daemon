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

#include "config.h"
#include "emer-permissions-provider.h"
#include "mock-permissions-provider.h"

#include <gio/gio.h>
#include <glib.h>

typedef struct _EmerPermissionsProvider
{
  GObject parent;

  gboolean daemon_enabled;
  gboolean uploading_enabled;

  gchar *server_url;
} EmerPermissionsProvider;

G_DEFINE_TYPE (EmerPermissionsProvider, emer_permissions_provider, G_TYPE_OBJECT)

static void
emer_permissions_provider_finalize (GObject *object)
{
  EmerPermissionsProvider *self = EMER_PERMISSIONS_PROVIDER (object);

  g_clear_pointer (&self->server_url, g_free);

  G_OBJECT_CLASS (emer_permissions_provider_parent_class)->finalize (object);
}

static void
emer_permissions_provider_class_init (EmerPermissionsProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = emer_permissions_provider_finalize;

}

static void
emer_permissions_provider_init (EmerPermissionsProvider *self)
{
  self->daemon_enabled = TRUE;
  self->uploading_enabled = TRUE;
  self->server_url = NULL;
}

/* MOCK PUBLIC API */

EmerPermissionsProvider *
emer_permissions_provider_new (void)
{
  return g_object_new (EMER_TYPE_PERMISSIONS_PROVIDER, NULL);
}

EmerPermissionsProvider *
emer_permissions_provider_new_full (const gchar *config_file_path,
                                    const gchar *ostree_config_file_path)
{
  return emer_permissions_provider_new ();
}

EmerPermissionsProvider *
mock_permissions_provider_new (const gchar *server_url)
{
  EmerPermissionsProvider *self = emer_permissions_provider_new ();
  self->server_url = g_strdup (server_url);
  return self;
}

gboolean
emer_permissions_provider_get_daemon_enabled (EmerPermissionsProvider *self)
{
  return self->daemon_enabled;
}

void
emer_permissions_provider_set_daemon_enabled (EmerPermissionsProvider *self,
                                              gboolean                 enabled)
{
  self->daemon_enabled = enabled;

  /* Emit a property notification even though there isn't a property by this
   * name in this mock object.
   */
  g_signal_emit_by_name (self, "notify::daemon-enabled", NULL);
}

gboolean
emer_permissions_provider_get_uploading_enabled (EmerPermissionsProvider *self)
{
  return self->uploading_enabled;
}

gchar *
emer_permissions_provider_get_environment (EmerPermissionsProvider *self)
{
  /* The real class emits these signals whenever this function is called,
   * regardless of whether the values have changed. This weird behaviour
   * led to a bug where the daemon would crash on startup if it was disabled.
   *
   * Replicate this behaviour here.
   */
  g_signal_emit_by_name (self, "notify::daemon-enabled", NULL);
  g_signal_emit_by_name (self, "notify::uploading-enabled", NULL);

  return g_strdup ("test");
}

gchar *
emer_permissions_provider_get_server_url (EmerPermissionsProvider *self)
{
  return g_strdup (self->server_url);
}

/* API OF MOCK OBJECT */

/* Sets the value to return from
 * emer_permissions_provider_get_uploading_enabled(). */
void
emer_permissions_provider_set_uploading_enabled (EmerPermissionsProvider *self,
                                                 gboolean                 uploading_enabled)
{
  self->uploading_enabled = uploading_enabled;

  /* Emit a property notification even though there isn't a property by this
   * name in this mock object.
   */
  g_signal_emit_by_name (self, "notify::uploading-enabled", NULL);
}
