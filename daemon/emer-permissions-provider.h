/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef EMER_PERMISSIONS_PROVIDER_H
#define EMER_PERMISSIONS_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_PERMISSIONS_PROVIDER emer_permissions_provider_get_type()

#define EMER_PERMISSIONS_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMER_TYPE_PERMISSIONS_PROVIDER, EmerPermissionsProvider))

#define EMER_PERMISSIONS_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMER_TYPE_PERMISSIONS_PROVIDER, EmerPermissionsProviderClass))

#define EMER_IS_PERMISSIONS_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMER_TYPE_PERMISSIONS_PROVIDER))

#define EMER_IS_PERMISSIONS_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMER_TYPE_PERMISSIONS_PROVIDER))

#define EMER_PERMISSIONS_PROVIDER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMER_TYPE_PERMISSIONS_PROVIDER, EmerPermissionsProviderClass))

typedef struct _EmerPermissionsProvider EmerPermissionsProvider;
typedef struct _EmerPermissionsProviderClass EmerPermissionsProviderClass;

struct _EmerPermissionsProvider
{
  GObject parent;
};

struct _EmerPermissionsProviderClass
{
  GObjectClass parent_class;
};

GType                    emer_permissions_provider_get_type           (void) G_GNUC_CONST;

EmerPermissionsProvider *emer_permissions_provider_new                (void);

EmerPermissionsProvider *emer_permissions_provider_new_full           (const gchar             *config_file_path);

gboolean                 emer_permissions_provider_get_daemon_enabled (EmerPermissionsProvider *self);

void                     emer_permissions_provider_set_daemon_enabled (EmerPermissionsProvider *self,
                                                                       gboolean                 enabled);

gchar                   *emer_permissions_provider_get_environment    (EmerPermissionsProvider *self);

G_END_DECLS

#endif /* EMER_PERMISSIONS_PROVIDER_H */
