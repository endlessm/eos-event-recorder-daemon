/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#ifndef EMTR_SENDER_H
#define EMTR_SENDER_H

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

#include "emtr-types.h"
#include "emtr-connection.h"

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define EMTR_TYPE_SENDER emtr_sender_get_type()

#define EMTR_SENDER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMTR_TYPE_SENDER, EmtrSender))

#define EMTR_SENDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMTR_TYPE_SENDER, EmtrSenderClass))

#define EMTR_IS_SENDER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMTR_TYPE_SENDER))

#define EMTR_IS_SENDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMTR_TYPE_SENDER))

#define EMTR_SENDER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMTR_TYPE_SENDER, EmtrSenderClass))

/**
 * EmtrSender:
 *
 * This instance structure contains no public members.
 */
typedef struct _EmtrSender EmtrSender;
/**
 * EmtrSenderClass:
 *
 * This class structure contains no public members.
 */
typedef struct _EmtrSenderClass EmtrSenderClass;

struct _EmtrSender
{
  /*< private >*/
  GObject parent;
};

struct _EmtrSenderClass
{
  /*< private >*/
  GObjectClass parent_class;

  /* For future expansion */
  gpointer _padding[8];
};

EMTR_ALL_API_VERSIONS
GType           emtr_sender_get_type         (void) G_GNUC_CONST;

EMTR_ALL_API_VERSIONS
EmtrSender     *emtr_sender_new              (GFile          *storage_file);

EMTR_ALL_API_VERSIONS
GFile          *emtr_sender_get_storage_file (EmtrSender     *self);

EMTR_ALL_API_VERSIONS
EmtrConnection *emtr_sender_get_connection   (EmtrSender     *self);

EMTR_ALL_API_VERSIONS
void            emtr_sender_set_connection   (EmtrSender     *self,
                                              EmtrConnection *connection);

EMTR_ALL_API_VERSIONS
gboolean        emtr_sender_send_data        (EmtrSender     *self,
                                              GVariant       *payload,
                                              GCancellable   *cancellable,
                                              GError        **error);

G_END_DECLS

#endif /* EMTR_SENDER_H */
