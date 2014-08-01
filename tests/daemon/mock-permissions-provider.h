/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef MOCK_PERMISSIONS_PROVIDER_H
#define MOCK_PERMISSIONS_PROVIDER_H

#include <glib.h>

#include "emer-permissions-provider.h"

G_BEGIN_DECLS

gint mock_permissions_provider_get_daemon_enabled_called (EmerPermissionsProvider *self);

G_END_DECLS

#endif /* MOCK_PERMISSIONS_PROVIDER_H */
