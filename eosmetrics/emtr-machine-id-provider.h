/* emtr-machine-id-provider.h */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef __EMTR_MACHINE_ID_PROVIDER_H__
#define __EMTR_MACHINE_ID_PROVIDER_H__

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

#include "emtr-types.h"
#include <glib-object.h>
#include <gio/gio.h>
#include <string.h>

G_BEGIN_DECLS

#define EMTR_TYPE_MACHINE_ID_PROVIDER emtr_machine_id_provider_get_type()

#define EMTR_MACHINE_ID_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMTR_TYPE_MACHINE_ID_PROVIDER, EmtrMachineIdProvider))

#define EMTR_MACHINE_ID_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMTR_TYPE_MACHINE_ID_PROVIDER, EmtrMachineIdProviderClass))

#define EMTR_IS_MACHINE_ID_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMTR_TYPE_MACHINE_ID_PROVIDER))

#define EMTR_IS_MACHINE_ID_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMTR_TYPE_MACHINE_ID_PROVIDER))

#define EMTR_MACHINE_ID_PROVIDER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMTR_TYPE_MACHINE_ID_PROVIDER, EmtrMachineIdProviderClass))

/**
 * EmtrMachineIdProvider:
 *
 * This instance structure contains no public members.
 */
typedef struct _EmtrMachineIdProvider EmtrMachineIdProvider;

/**
 * EmtrMachineIdProviderClass:
 *
 * This class structure contains no public members.
 */
typedef struct _EmtrMachineIdProviderClass EmtrMachineIdProviderClass;


struct _EmtrMachineIdProvider
{
  /*< private >*/
  GObject parent;
};

struct _EmtrMachineIdProviderClass
{
  /*< private >*/
  GObjectClass parent_class;
};

EMTR_ALL_API_VERSIONS
GType emtr_machine_id_provider_get_type (void) G_GNUC_CONST;

EMTR_ALL_API_VERSIONS
EmtrMachineIdProvider *emtr_machine_id_provider_new         (const gchar           *machine_id_file_path);

EMTR_ALL_API_VERSIONS
EmtrMachineIdProvider *emtr_machine_id_provider_get_default (void);

EMTR_ALL_API_VERSIONS
gboolean               emtr_machine_id_provider_get_id      (EmtrMachineIdProvider *self,
                                                             guchar                 uuid[16]);

G_END_DECLS

#endif /* __EMTR_MACHINE_ID_PROVIDER_H__ */
