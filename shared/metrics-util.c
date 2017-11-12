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

#include "metrics-util.h"

#include <byteswap.h>
#include <uuid/uuid.h>

#include <glib.h>
#include <gio/gio.h>

#define UUID_SERIALIZED_LEN 37

guint64
swap_bytes_64_if_big_endian (guint64 value)
{
  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    return bswap_64 (value);
  if (G_BYTE_ORDER != G_LITTLE_ENDIAN)
    g_error ("This machine is neither big endian nor little endian. Mixed-"
             "endian machines are not supported by the metrics system.");
  return value;
}

/*
 * Returns a new reference to a little-endian version of the given GVariant
 * regardless of this machine's endianness. Crashes with a g_error if this
 * machine is middle-endian (a.k.a., mixed-endian).
 *
 * The returned GVariant should have g_variant_unref() called on it when it is
 * no longer needed.
 */
GVariant *
swap_bytes_if_big_endian (GVariant *variant)
{
  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    return g_variant_byteswap (variant);

  if (G_BYTE_ORDER != G_LITTLE_ENDIAN)
    g_error ("Holy crap! This machine is neither big NOR little-endian, time "
             "to panic. AAHAHAHAHAH!");

  return g_variant_ref_sink (variant);
}

/*
 * Initializes the given uuid_builder and populates it with the contents of
 * uuid.
 */
void
get_uuid_builder (uuid_t           uuid,
                  GVariantBuilder *uuid_builder)
{
  g_variant_builder_init (uuid_builder, G_VARIANT_TYPE_BYTESTRING);
  for (size_t i = 0; i < UUID_LENGTH; ++i)
    g_variant_builder_add (uuid_builder, "y", uuid[i]);
}

GVariant *
deep_copy_variant (GVariant *variant)
{
  GBytes *bytes = g_variant_get_data_as_bytes (variant);
  const GVariantType *variant_type = g_variant_get_type (variant);
  gboolean trusted = g_variant_is_normal_form (variant);
  GVariant *copy = g_variant_new_from_bytes (variant_type, bytes, trusted);
  g_bytes_unref (bytes);
  return copy;
}

void
destroy_variants (GVariant **variants,
                  gsize      num_variants)
{
  for (gsize i = 0; i < num_variants; i++)
    g_variant_unref (variants[i]);

  g_free (variants);
}

gboolean
write_tracking_id_file (const gchar  *path,
                        GError      **error)
{
  uuid_t override_machine_id;
  gchar serialized_override_machine_id[UUID_SERIALIZED_LEN];
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GFile) directory = g_file_get_parent (file);
  g_autoptr(GError) local_error = NULL;

  uuid_clear (override_machine_id);
  uuid_generate (override_machine_id);
  uuid_unparse (override_machine_id, serialized_override_machine_id);

  if (!g_file_make_directory_with_parents (directory, NULL, &local_error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }

      g_clear_error (&local_error);
    }

  if (!g_file_set_contents (path, serialized_override_machine_id, -1, error))
    return FALSE;

  return TRUE;
}
