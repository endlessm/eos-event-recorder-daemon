/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#ifndef EMTR_CONNECTION_H
#define EMTR_CONNECTION_H

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

#include "emtr-types.h"

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define EMTR_TYPE_CONNECTION emtr_connection_get_type()

#define EMTR_CONNECTION(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMTR_TYPE_CONNECTION, EmtrConnection))

#define EMTR_CONNECTION_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMTR_TYPE_CONNECTION, EmtrConnectionClass))

#define EMTR_IS_CONNECTION(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMTR_TYPE_CONNECTION))

#define EMTR_IS_CONNECTION_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMTR_TYPE_CONNECTION))

#define EMTR_CONNECTION_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMTR_TYPE_CONNECTION, EmtrConnectionClass))

/**
 * EmtrConnection:
 *
 * This instance structure contains no public members.
 */
typedef struct _EmtrConnection EmtrConnection;
/**
 * EmtrConnectionClass:
 *
 * This class structure contains no public members.
 */
typedef struct _EmtrConnectionClass EmtrConnectionClass;

struct _EmtrConnection
{
  /*< private >*/
  GObject parent;

  /**
   * _uuid_gen_func: (skip)
   *
   * For testing only.
   */
  gchar *(*_uuid_gen_func) (void);
  /**
   * _mac_gen_func: (skip)
   *
   * For testing only.
   */
  gint64 (*_mac_gen_func) (void);
  /**
   * _web_send_func: (skip)
   *
   * For testing only.
   */
  gboolean (*_web_send_func) (const gchar *uri,
                              const gchar *data,
                              const gchar *user,
                              const gchar *pass,
                              GError **error);
};

struct _EmtrConnectionClass
{
  /*< private >*/
  GObjectClass parent_class;

  /* For future expansion */
  gpointer _padding[8];
};

EMTR_ALL_API_VERSIONS
GType           emtr_connection_get_type                 (void) G_GNUC_CONST;

EMTR_ALL_API_VERSIONS
EmtrConnection *emtr_connection_new                      (const gchar    *uri_context,
                                                          const gchar    *form_param_name,
                                                          GFile          *endpoint_config_file,
                                                          GFile          *fingerprint_file);

EMTR_ALL_API_VERSIONS
const gchar    *emtr_connection_get_uri_context          (EmtrConnection *self);

EMTR_ALL_API_VERSIONS
const gchar    *emtr_connection_get_form_param_name      (EmtrConnection *self);

EMTR_ALL_API_VERSIONS
GFile          *emtr_connection_get_endpoint_config_file (EmtrConnection *self);

EMTR_ALL_API_VERSIONS
GFile          *emtr_connection_get_fingerprint_file     (EmtrConnection *self);

EMTR_ALL_API_VERSIONS
const gchar    *emtr_connection_get_endpoint             (EmtrConnection *self);

EMTR_ALL_API_VERSIONS
gboolean        emtr_connection_send                     (EmtrConnection *self,
                                                          GVariant       *payload,
                                                          GError        **error);

G_END_DECLS

#endif /* EMTR_METRICSCONNECTION_H */
