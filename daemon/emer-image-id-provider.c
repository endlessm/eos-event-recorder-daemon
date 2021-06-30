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

#include "emer-image-id-provider.h"
#include <glib.h>
#include <sys/xattr.h>

/*
 * This is a string such as "eos-eos3.1-amd64-amd64.170115-071322.base" which is
 * saved in an attribute on the root filesystem by the image builder, and allows
 * us to tell the channel that the OS was installed by (eg download, OEM
 * pre-install, Endless hardware, USB stick, etc) and which version was
 * installed. The payload is a single string containing this image ID,
 * if present.
 */

#define EOS_IMAGE_VERSION_EVENT "6b1c1cfc-bc36-438c-0647-dacd5878f2b3"

#define EOS_IMAGE_VERSION_XATTR "user.eos-image-version"
#define EOS_IMAGE_VERSION_PATH "/sysroot"
#define EOS_IMAGE_VERSION_ALT_PATH "/"

static gchar *
get_image_version_for_path (const gchar *path)
{
  ssize_t xattr_size;
  g_autofree gchar *image_version = NULL;

  xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR, NULL, 0);

  if (xattr_size < 0 || xattr_size > SSIZE_MAX - 1)
    return NULL;

  image_version = g_malloc0 (xattr_size + 1);

  xattr_size = getxattr (path, EOS_IMAGE_VERSION_XATTR,
                         image_version, xattr_size);

  /* this check is primarily for ERANGE, in case the attribute size has
   * changed from the first call to this one */
  if (xattr_size < 0)
    {
      g_warning ("Error when getting 'eos-image-version' from %s: %s", path,
                 strerror (errno));
      return NULL;
    }

  /* shouldn't happen, but if the filesystem is modified or corrupted, we
   * don't want to cause assertion errors / D-Bus disconnects with invalid
   * UTF-8 strings */
  if (!g_utf8_validate (image_version, xattr_size, NULL))
    {
      g_warning ("Invalid UTF-8 when getting 'eos-image-version' from %s",
                 path);
      return NULL;
    }

  return g_steal_pointer (&image_version);
}

/*
 * emer_image_id_provider_get_version:
 *
 * Retrieves the image version string saved by the image builder from the root
 * filesystem.
 *
 * Returns: a string of the image version.
 */
gchar *
emer_image_id_provider_get_version (void)
{
  gchar *image_version = get_image_version_for_path (EOS_IMAGE_VERSION_PATH);

  if (image_version == NULL)
    image_version = get_image_version_for_path (EOS_IMAGE_VERSION_ALT_PATH);

  return image_version;
}
