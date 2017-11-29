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

#ifndef MOCK_CIRCULAR_FILE_H
#define MOCK_CIRCULAR_FILE_H

#include <glib-object.h>

#include "emer-circular-file.h"

G_BEGIN_DECLS

void                 mock_circular_file_set_construct_error     (const GError             *error);
gboolean             mock_circular_file_got_reinitialize        (void);

G_END_DECLS

#endif /* MOCK_CIRCULAR_FILE_H */
