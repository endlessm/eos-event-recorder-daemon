/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2021 Endless OS Foundation LLC. */

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

#ifndef EMER_IMAGE_ID_PROVIDER_H
#define EMER_IMAGE_ID_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define IMAGE_VERSION	"eos-eos3.1-amd64-amd64.170115-071322.base"
#define OS_VERSION	"3.1.0"

gchar *emer_image_id_provider_get_version (void);

gchar *emer_image_id_provider_get_os_version(void);

G_END_DECLS

#endif /* EMER_IMAGE_ID_PROVIDER_H */
