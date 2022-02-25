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

#include "config.h"
#include "metrics-util.h"

#include <byteswap.h>
#include <uuid/uuid.h>

#include <glib.h>

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

GVariant *
get_uuid_as_variant (uuid_t uuid)
{
  return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, uuid, UUID_LENGTH, sizeof (uuid[0]));
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
