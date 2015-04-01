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

#ifndef EMER_NETWORK_SEND_PROVIDER_H
#define EMER_NETWORK_SEND_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_NETWORK_SEND_PROVIDER emer_network_send_provider_get_type()

#define EMER_NETWORK_SEND_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMER_TYPE_NETWORK_SEND_PROVIDER, EmerNetworkSendProvider))

#define EMER_NETWORK_SEND_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMER_TYPE_NETWORK_SEND_PROVIDER, EmerNetworkSendProviderClass))

#define EMER_IS_NETWORK_SEND_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMER_TYPE_NETWORK_SEND_PROVIDER))

#define EMER_IS_NETWORK_SEND_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMER_TYPE_NETWORK_SEND_PROVIDER))

#define EMER_NETWORK_SEND_PROVIDER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMER_TYPE_NETWORK_SEND_PROVIDER, EmerNetworkSendProviderClass))

/*
 * EmerNetworkSendProvider:
 *
 * This instance structure contains no public members.
 */
typedef struct _EmerNetworkSendProvider EmerNetworkSendProvider;

/*
 * EmerNetworkSendProvider:
 *
 * This class structure contains no public members.
 */
typedef struct _EmerNetworkSendProviderClass EmerNetworkSendProviderClass;

struct _EmerNetworkSendProvider
{
  /*< private >*/
  GObject parent;
};

struct _EmerNetworkSendProviderClass
{
  /*< private >*/
  GObjectClass parent_class;
};

GType                    emer_network_send_provider_get_type              (void) G_GNUC_CONST;

EmerNetworkSendProvider *emer_network_send_provider_new                   (void);

EmerNetworkSendProvider *emer_network_send_provider_new_full              (const gchar             *path);

gint                     emer_network_send_provider_get_send_number       (EmerNetworkSendProvider *self);

void                     emer_network_send_provider_increment_send_number (EmerNetworkSendProvider *self);

G_END_DECLS

#endif /* EMER_NETWORK_SEND_PROVIDER_H */
