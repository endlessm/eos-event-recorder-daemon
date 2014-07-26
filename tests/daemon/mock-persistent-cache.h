/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef MOCK_PESISTENT_CACHE_H
#define MOCK_PERSISTENT_CACHE_H

#include <glib-object.h>

#include "emer-persistent-cache.h"

G_BEGIN_DECLS

guint mock_persistent_cache_get_store_metrics_called (EmerPersistentCache *self);

G_END_DECLS

#endif /* MOCK_PERSISTENT_CACHE_H */
