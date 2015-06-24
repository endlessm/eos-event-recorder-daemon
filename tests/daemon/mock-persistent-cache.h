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

#ifndef MOCK_PESISTENT_CACHE_H
#define MOCK_PERSISTENT_CACHE_H

#include <glib-object.h>

#include "emer-persistent-cache.h"

G_BEGIN_DECLS

#define BOOT_TIME_OFFSET G_GINT64_CONSTANT (73)
#define MAX_NUM_VARIANTS 10

gint mock_persistent_cache_get_num_timestamp_updates (EmerPersistentCache *self);

G_END_DECLS

#endif /* MOCK_PERSISTENT_CACHE_H */
