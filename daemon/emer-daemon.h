/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef EMER_DAEMON_H
#define EMER_DAEMON_H

#include "emer-machine-id-provider.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"

#include <glib-object.h>

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
};

GType                    emer_daemon_get_type                 (void) G_GNUC_CONST;

EmerDaemon *             emer_daemon_new                      (const gchar             *environment);

EmerDaemon *             emer_daemon_new_full                 (GRand                   *rand,
                                                               gint                     version_number,
                                                               guint                    network_send_interval,
                                                               const gchar             *proxy_server_uri,
                                                               EmerMachineIdProvider   *machine_id_provider,
                                                               EmerPermissionsProvider *permissions_provider,
                                                               EmerPersistentCache     *persistent_cache,
                                                               gint                     buffer_length);

void                     emer_daemon_record_singular_event    (EmerDaemon              *self,
                                                               guint32                  user_id,
                                                               GVariant                *event_id,
                                                               gint64                   relative_timestamp,
                                                               gboolean                 has_payload,
                                                               GVariant                *payload);

void                     emer_daemon_record_aggregate_event   (EmerDaemon              *self,
                                                               guint32                  user_id,
                                                               GVariant                *event_id,
                                                               gint64                   count,
                                                               gint64                   relative_timestamp,
                                                               gboolean                 has_payload,
                                                               GVariant                *payload);

void                     emer_daemon_record_event_sequence    (EmerDaemon              *self,
                                                               guint32                  user_id,
                                                               GVariant                *event_id,
                                                               GVariant                *events);

EmerPermissionsProvider *emer_daemon_get_permissions_provider (EmerDaemon              *self);

G_END_DECLS

#endif /* EMER_DAEMON_H */
