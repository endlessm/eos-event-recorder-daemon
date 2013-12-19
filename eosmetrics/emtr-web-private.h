/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#ifndef EMTR_WEB_H
#define EMTR_WEB_H

#include "emtr-types.h"

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

gboolean emtr_web_post_authorized_sync   (const gchar        *uri,
                                          const gchar        *json_data,
                                          const gchar        *username,
                                          const gchar        *password,
                                          GCancellable       *cancellable,
                                          GError            **error);

void     emtr_web_post_authorized        (const gchar        *uri,
                                          const gchar        *json_data,
                                          const gchar        *username,
                                          const gchar        *password,
                                          GCancellable       *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer            user_data);

gboolean emtr_web_post_authorized_finish (GAsyncResult       *result,
                                          GError            **error);

G_END_DECLS

#endif /* EMTR_WEB_H */
