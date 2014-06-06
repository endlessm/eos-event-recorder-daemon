/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef EMER_MACHINE_ID_PROVIDER_H
#define EMER_MACHINE_ID_PROVIDER_H

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

GType                  emer_machine_id_provider_get_type    (void) G_GNUC_CONST;

EmerMachineIdProvider *emer_machine_id_provider_new         (const gchar           *machine_id_file_path);

EmerMachineIdProvider *emer_machine_id_provider_get_default (void);

gboolean               emer_machine_id_provider_get_id      (EmerMachineIdProvider *self,
                                                             guchar                 uuid[16]);

G_END_DECLS

#endif /* EMER_MACHINE_ID_PROVIDER_H */
