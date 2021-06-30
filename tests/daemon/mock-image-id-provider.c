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

#include "mock-image-id-provider.h"
#include <glib.h>
#include <string.h>

/*
 * Recorded once at startup to report the image ID. This is a string such as
 * "eos-eos3.1-amd64-amd64.170115-071322.base" which is saved in an attribute
 * on the root filesystem by the image builder, and allows us to tell the
 * channel that the OS was installed by (eg download, OEM pre-install, Endless
 * hardware, USB stick, etc) and which version was installed. The payload
 * is a single string containing this image ID, if present.
 */

gchar *
emer_image_id_provider_get_version (void)
{
  return g_strdup (IMAGE_VERSION);
}
