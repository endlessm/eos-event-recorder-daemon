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

#include "emer-aggregate-tally.h"
#include "emer-event-recorder-server.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"

G_BEGIN_DECLS

#define EMER_TYPE_DAEMON emer_daemon_get_type()

#define EMER_DAEMON(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMER_TYPE_DAEMON, EmerDaemon))

#define EMER_DAEMON_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMER_TYPE_DAEMON, EmerDaemonClass))

#define EMER_IS_DAEMON(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMER_TYPE_DAEMON))

#define EMER_IS_DAEMON_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMER_TYPE_DAEMON))

#define EMER_DAEMON_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMER_TYPE_DAEMON, EmerDaemonClass))

typedef struct _EmerDaemon EmerDaemon;
typedef struct _EmerDaemonClass EmerDaemonClass;

struct _EmerDaemon
{
  GObject parent;
};

struct _EmerDaemonClass
{
  GObjectClass parent_class;

  void (*upload_finished_handler) (EmerDaemon *self);
};

GType                    emer_daemon_get_type                 (void) G_GNUC_CONST;

EmerDaemon *             emer_daemon_new                      (const gchar             *persistent_cache_directory,
                                                               EmerPermissionsProvider *permissions_provider);

EmerDaemon *             emer_daemon_new_full                 (GRand                   *rand,
                                                               const gchar             *server_uri,
                                                               guint                    network_send_interval,
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
