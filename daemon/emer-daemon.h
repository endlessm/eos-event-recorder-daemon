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

#ifndef EMER_DAEMON_H
#define EMER_DAEMON_H

#include <gio/gio.h>
#include <glib-object.h>

#include "shared/emer-clock.h"

#include "emer-aggregate-tally.h"
#include "emer-event-recorder-server.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"

G_BEGIN_DECLS

#define EMER_TYPE_DAEMON emer_daemon_get_type()
G_DECLARE_FINAL_TYPE (EmerDaemon, emer_daemon, EMER, DAEMON, GObject)

EmerDaemon *             emer_daemon_new                      (const gchar             *persistent_cache_directory,
                                                               EmerPermissionsProvider *permissions_provider);

EmerDaemon *             emer_daemon_new_full                 (GRand                   *rand,
                                                               const gchar             *server_uri,
                                                               guint                    network_send_interval,
                                                               EmerClock               *clock,
                                                               GNetworkMonitor         *network_monitor,
                                                               EmerPermissionsProvider *permissions_provider,
                                                               EmerPersistentCache     *persistent_cache,
                                                               EmerAggregateTally      *aggregate_tally,
                                                               gulong                   max_bytes_buffered);

void                     emer_daemon_record_singular_event    (EmerDaemon              *self,
                                                               GVariant                *event_id,
                                                               gint64                   relative_timestamp,
                                                               gboolean                 has_payload,
                                                               GVariant                *payload);

void                     emer_daemon_enqueue_aggregate_event  (EmerDaemon              *self,
                                                               GVariant                *event_id,
                                                               const char              *period_start,
                                                               guint32                  count,
                                                               GVariant                *payload);

void                     emer_daemon_record_event_sequence    (EmerDaemon              *self,

                                                               guint32                  user_id,
                                                               GVariant                *event_id,
                                                               GVariant                *events);

void                     emer_daemon_upload_events            (EmerDaemon              *self,
                                                               GAsyncReadyCallback      callback,
                                                               gpointer                 user_data);

gboolean                 emer_daemon_upload_events_finish     (EmerDaemon              *self,
                                                               GAsyncResult            *result,
                                                               GError                 **error);
EmerPermissionsProvider *emer_daemon_get_permissions_provider (EmerDaemon              *self);

gboolean                 emer_daemon_start_aggregate_timer    (EmerDaemon              *self,
                                                               GDBusConnection         *connection,
                                                               const gchar             *sender_name,
                                                               guint32                  unix_user_id,
                                                               GVariant                *event_id,
                                                               gboolean                 has_payload,
                                                               GVariant                *payload,
                                                               gchar                  **out_timer_object_path,
                                                               GError                 **error);

void                     emer_daemon_shutdown                 (EmerDaemon              *self);

G_END_DECLS

#endif /* EMER_DAEMON_H */
