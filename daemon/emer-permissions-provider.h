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

#ifndef EMER_PERMISSIONS_PROVIDER_H
#define EMER_PERMISSIONS_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_PERMISSIONS_PROVIDER emer_permissions_provider_get_type()
G_DECLARE_FINAL_TYPE (EmerPermissionsProvider,
                      emer_permissions_provider,
                      EMER,
                      PERMISSIONS_PROVIDER,
                      GObject)

EmerPermissionsProvider *emer_permissions_provider_new                   (void);

EmerPermissionsProvider *emer_permissions_provider_new_full              (const gchar             *config_file_path,
                                                                          const gchar             *ostree_config_file_path);

gboolean                 emer_permissions_provider_get_daemon_enabled    (EmerPermissionsProvider *self);

void                     emer_permissions_provider_set_daemon_enabled    (EmerPermissionsProvider *self,
                                                                          gboolean                 enabled);

gboolean                 emer_permissions_provider_get_uploading_enabled (EmerPermissionsProvider *self);

void                     emer_permissions_provider_set_uploading_enabled (EmerPermissionsProvider *self,
                                                                          gboolean                 enabled);

gchar                   *emer_permissions_provider_get_environment       (EmerPermissionsProvider *self);

gchar                   *emer_permissions_provider_get_server_url        (EmerPermissionsProvider *self);

G_END_DECLS

#endif /* EMER_PERMISSIONS_PROVIDER_H */
