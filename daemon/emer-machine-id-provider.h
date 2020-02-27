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

#ifndef EMER_MACHINE_ID_PROVIDER_H
#define EMER_MACHINE_ID_PROVIDER_H

#include <uuid/uuid.h>

#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_MACHINE_ID_PROVIDER emer_machine_id_provider_get_type()

#define EMER_MACHINE_ID_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMER_TYPE_MACHINE_ID_PROVIDER, EmerMachineIdProvider))

#define EMER_MACHINE_ID_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMER_TYPE_MACHINE_ID_PROVIDER, EmerMachineIdProviderClass))

#define EMER_IS_MACHINE_ID_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMER_TYPE_MACHINE_ID_PROVIDER))

#define EMER_IS_MACHINE_ID_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMER_TYPE_MACHINE_ID_PROVIDER))

#define EMER_MACHINE_ID_PROVIDER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMER_TYPE_MACHINE_ID_PROVIDER, EmerMachineIdProviderClass))

/*
 * EmerMachineIdProvider:
 *
 * This instance structure contains no public members.
 */
typedef struct _EmerMachineIdProvider EmerMachineIdProvider;

/*
 * EmerMachineIdProviderClass:
 *
 * This class structure contains no public members.
 */
typedef struct _EmerMachineIdProviderClass EmerMachineIdProviderClass;


struct _EmerMachineIdProvider
{
  /*< private >*/
  GObject parent;
};

struct _EmerMachineIdProviderClass
{
  /*< private >*/
  GObjectClass parent_class;
};

GType                  emer_machine_id_provider_get_type (void) G_GNUC_CONST;

EmerMachineIdProvider *emer_machine_id_provider_new      (void);

EmerMachineIdProvider *emer_machine_id_provider_new_full (const gchar *tracking_id_path);

gboolean               emer_machine_id_provider_get_id   (EmerMachineIdProvider *self,
                                                          gchar                **machine_id_hex,
                                                          uuid_t                 machine_id);
gboolean               emer_machine_id_provider_reset_tracking_id (EmerMachineIdProvider  *self,
                                                                   GError                **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EmerMachineIdProvider, g_object_unref);

G_END_DECLS

#endif /* EMER_MACHINE_ID_PROVIDER_H */
