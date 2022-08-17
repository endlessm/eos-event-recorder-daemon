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

#ifndef METRICS_UTIL_H
#define METRICS_UTIL_H

#include <glib.h>
#include <uuid/uuid.h>

/*
 * The number of elements in a uuid_t. uuid_t is assumed to be a fixed-length
 * array of guchar.
 */
#define UUID_LENGTH (sizeof (uuid_t) / sizeof (guchar))

G_BEGIN_DECLS

guint64   swap_bytes_64_if_big_endian (guint64          value);

GVariant *swap_bytes_if_big_endian    (GVariant        *variant);

GVariant *get_uuid_as_variant         (uuid_t uuid);

GVariant *deep_copy_variant           (GVariant        *variant);

void      destroy_variants            (GVariant       **variants,
                                       gsize            num_variants);

G_END_DECLS

#endif /* METRICS_UTIL_H */
