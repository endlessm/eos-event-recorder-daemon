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

#include "emer-site-id-provider.h"
#include <glib.h>

/*
 * Recorded from location.conf. The auxiliary payload is a dictionary of string
 * keys (such as facility, city and state) to the values provided in the
 * location.conf file. The intention is to allow an operator to provide an
 * optional human-readable label for the location of the system, which can be
 * used when preparing reports or visualisations of the metrics data.
 */

#define LOCATION_CONF_FILE SYSCONFDIR "/metrics/location.conf"
#define LOCATION_LABEL_GROUP "Label"

static GVariant *
emer_read_location_label (GKeyFile *kf)
{
  g_auto (GStrv) keys = g_key_file_get_keys (kf, LOCATION_LABEL_GROUP, NULL, NULL);
  if (keys == NULL || *keys == NULL)
    return NULL;

  g_auto (GVariantBuilder) builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_ARRAY);
  gboolean seen_nonempty_value = FALSE;
  for (GStrv cur = keys; *cur != NULL; cur++)
    {
      const gchar *key = *cur;
      g_autofree gchar *val = g_key_file_get_string (kf, LOCATION_LABEL_GROUP, key, NULL);

      if (val == NULL || *val == '\0')
        continue;

      seen_nonempty_value = TRUE;
      g_variant_builder_add (&builder, "{ss}", key, val);
    }

  if (!seen_nonempty_value)
    return NULL;

  return g_variant_builder_end (&builder);
}

/*
 * emer_site_id_provider_get_id:
 *
 * Retrieves the site information provided by Metrics.
 *
 * Returns: a pointer of GVariant saving the site information in an array of
 * dictionary format.
 */
GVariant *
emer_site_id_provider_get_id (void)
{
  g_autoptr (GError) err = NULL;
  g_autoptr (GKeyFile) kf = g_key_file_new ();

  if (!g_key_file_load_from_file (kf, LOCATION_CONF_FILE, G_KEY_FILE_NONE, &err))
    {
      g_warning ("Failed to load " LOCATION_CONF_FILE ", unable to record location label: %s",
                 err->message);
      return NULL;
    }

  return emer_read_location_label(kf);
}
