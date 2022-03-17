/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2017 Endless Mobile, Inc. */

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

#ifndef EMER_TYPES_H
#define EMER_TYPES_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    EMER_ERROR_METRICS_DISABLED,
    EMER_ERROR_UPLOADING_DISABLED,
    EMER_ERROR_INVALID_MACHINE_ID,
    EMER_ERROR_INVALID_EVENT_ID,
    EMER_ERROR_LAST = EMER_ERROR_INVALID_EVENT_ID, /*< skip >*/
} EmerError;

#define EMER_ERROR (emer_error_quark ())
GQuark emer_error_quark (void);

G_END_DECLS

#endif // EMER_TYPES_H
