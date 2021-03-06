/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 - 2016 Endless Mobile, Inc. */

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

#include "emer-machine-id-provider.h"
#include "emer-types.h"

#include <string.h>
#include <uuid/uuid.h>

#include <gio/gio.h>

#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <glib/gstrfuncs.h>

#include "shared/metrics-util.h"

typedef struct EmerMachineIdProviderPrivate
{
  gchar *tracking_id_path;
  uuid_t id;
  gboolean id_is_valid;
} EmerMachineIdProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerMachineIdProvider, emer_machine_id_provider, G_TYPE_OBJECT)

/*
 * The expected size in bytes of the file located at
 * #EmerMachineIdProvider:path.
 * According to http://www.freedesktop.org/software/systemd/man/machine-id.html
 * the file should be 32 lower-case hexadecimal characters followed by a
 * newline character.
 */
#define FILE_LENGTH 33

/*
 * Filepath where an overridden random UUID, separate from /etc/machine-id
 * is stored. This machine-id is read from this path and used as the
 * default tracking ID for metrics purposes.
 */
#define TRACKING_ID_PATH SYSCONFDIR "/metrics/tracking-id"

#define UUID_SERIALIZED_LEN 37

enum
{
  PROP_0,
  PROP_TRACKING_ID_PATH,
  NPROPS
};

static GParamSpec *emer_machine_id_provider_props[NPROPS] = { NULL, };

static gboolean write_tracking_id_file (const gchar *path, GError **error);

/*
 * SECTION:emer-machine-id-provider
 * @title: Machine ID Provider
 * @short_description: Provides unique machine identifiers.
 *
 * The machine ID provider supplies UUIDs which anonymously identify the
 * machine (not the user) sending metrics.
 * This class abstracts away how and where UUIDs are generated from by providing
 * a simple interface via emer_machine_id_provider_get_id() to whatever calling
 * code needs it.
 */

static void
emer_machine_id_provider_get_property (GObject    *object,
                                       guint       property_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  EmerMachineIdProvider *self = EMER_MACHINE_ID_PROVIDER (object);
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  switch (property_id)
    {
    case PROP_TRACKING_ID_PATH:
      g_value_set_string (value, priv->tracking_id_path);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_machine_id_provider_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  EmerMachineIdProvider *self = EMER_MACHINE_ID_PROVIDER (object);
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  switch (property_id)
    {
    case PROP_TRACKING_ID_PATH:
      priv->tracking_id_path = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_machine_id_provider_finalize (GObject *object)
{
  EmerMachineIdProvider *self = EMER_MACHINE_ID_PROVIDER (object);
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  g_free (priv->tracking_id_path);

  G_OBJECT_CLASS (emer_machine_id_provider_parent_class)->finalize (object);
}

static void
emer_machine_id_provider_class_init (EmerMachineIdProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = emer_machine_id_provider_get_property;
  object_class->set_property = emer_machine_id_provider_set_property;
  object_class->finalize = emer_machine_id_provider_finalize;

  /* Blurb string is good enough default documentation for this */
  emer_machine_id_provider_props[PROP_TRACKING_ID_PATH] =
    g_param_spec_string ("tracking-id-path", "Tracking ID file path",
                         "The path to where a mutable tracking ID is stored.",
                         TRACKING_ID_PATH,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_machine_id_provider_props);
}

static void
emer_machine_id_provider_init (EmerMachineIdProvider *self)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  priv->id_is_valid = FALSE;
}

/*
 * emer_machine_id_provider_new_full:
 * @tracking_id_path: A location for an tracking id path,
 *                    see #EmerMachineIdProvider:tracking-id-path
 *
 * For special cases which intends to upload metrics data other than the
 * host machine, or if you are creating a mock ID provider for unit
 * testing, use this to create the machine ID provider based on the
 * spcified ID path.
 *
 * For all normal uses, you should use emer_machine_id_provider_new()
 * instead.
 *
 * Returns: (transfer full): A new #EmerMachineIdProvider.
 * Free with g_object_unref() when done if using C.
 */
EmerMachineIdProvider *
emer_machine_id_provider_new_full (const gchar *tracking_id_path)
{
  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER,
                       "tracking-id-path", tracking_id_path,
                       NULL);
}

/*
 * emer_machine_id_provider_new:
 *
 * Gets the ID provider that you should use for obtaining a unique machine ID in
 * production code. Uses a default filepath for the machine-id and the
 * default path for the location of the override tracking code.
 *
 * Returns: (transfer full): a production #EmerMachineIdProvider.
 */
EmerMachineIdProvider *
emer_machine_id_provider_new (void)
{
  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER,
                       "tracking-id-path", TRACKING_ID_PATH,
                       NULL);
}

/*
 * Returns a newly-allocated copy of uuid_sans_hyphens with hyphens inserted at
 * the appropriate positions as defined by uuid_unparse(3).
 * uuid_sans_hyphens is expected to be exactly 32 bytes, excluding the terminal
 * null byte.
 * Any extra bytes are ignored.
 * The returned string is guaranteed to be null-terminated.
 */
static gchar *
hyphenate_uuid (gchar *uuid_sans_hyphens)
{
  return g_strdup_printf ("%.8s-%.4s-%.4s-%.4s-%.12s", uuid_sans_hyphens,
                          uuid_sans_hyphens + 8, uuid_sans_hyphens + 12,
                          uuid_sans_hyphens + 16, uuid_sans_hyphens + 20);
}

/*
 * Returns a newly-allocated copy of uuid_with_hyphens with hyphens removed at
 * the appropriate positions as defined by uuid_unparse(3).
 * uuid_with_hyphens is expected to be exactly 36 bytes, excluding the terminal
 * null byte.
 * Any extra bytes are ignored.
 * The returned string is guaranteed to be have a newline and be nul-terminated.
 */
static gchar *
dehyphenate_uuid (gchar *uuid_with_hyphens)
{
  return g_strdup_printf ("%.8s%.4s%.4s%.4s%.12s\n", uuid_with_hyphens,
                          uuid_with_hyphens + 9, uuid_with_hyphens + 14,
                          uuid_with_hyphens + 19, uuid_with_hyphens + 24);
}

static gboolean
read_one_machine_id (const gchar  *machine_id_path,
                     gchar       **machine_id_hex,
                     uuid_t        id,
                     GError      **error)
{
  g_autofree gchar *machine_id_sans_hyphens = NULL;
  gsize machine_id_sans_hyphens_length;
  gboolean read_succeeded =
    g_file_get_contents (machine_id_path, &machine_id_sans_hyphens,
                         &machine_id_sans_hyphens_length, error);
  if (!read_succeeded)
    return FALSE;

  if (strlen (machine_id_sans_hyphens) != machine_id_sans_hyphens_length)
    {
      g_set_error (error,
                   EMER_ERROR,
                   EMER_ERROR_INVALID_MACHINE_ID,
                   "Machine ID file (%s) contained null byte, but should be "
                   "hexadecimal.",
                   machine_id_path);
      return FALSE;
    }

  if (machine_id_sans_hyphens_length != FILE_LENGTH)
    {
      g_set_error (error,
                   EMER_ERROR,
                   EMER_ERROR_INVALID_MACHINE_ID,
                   "Machine ID file (%s) contained %" G_GSIZE_FORMAT " bytes, "
                   "but expected %d bytes.",
                   machine_id_path,
                   machine_id_sans_hyphens_length,
                   FILE_LENGTH);
      return FALSE;
    }

  gchar *hyphenated_machine_id = hyphenate_uuid (machine_id_sans_hyphens);

  gint parse_failed = uuid_parse (hyphenated_machine_id, id);
  g_free (hyphenated_machine_id);

  if (parse_failed != 0)
    {
      g_set_error (error,
                   EMER_ERROR,
                   EMER_ERROR_INVALID_MACHINE_ID,
                   "Machine ID file (%s) did not contain UUID.",
                   machine_id_path);
      return FALSE;
    }

  if (machine_id_hex)
    *machine_id_hex = g_strchomp (g_steal_pointer (&machine_id_sans_hyphens));

  return TRUE;
}

static gboolean
read_machine_id (EmerMachineIdProvider *self, gchar **machine_id_hex)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);
  g_autoptr(GError) local_error = NULL;
  uuid_t id;

  if (!read_one_machine_id (priv->tracking_id_path, machine_id_hex, id, &local_error))
    {
      if (g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_debug ("Tracking id file %s does not exist hence creating one.",
                   priv->tracking_id_path);
          g_clear_error (&local_error);

          if (!write_tracking_id_file (priv->tracking_id_path, &local_error))
            {
              g_message ("Failed to initialize tracking ID at %s: %s.",
                         priv->tracking_id_path,
                         local_error->message);
              return FALSE;
            }
	  else if (!read_one_machine_id (priv->tracking_id_path, machine_id_hex, id, &local_error))
            {
              g_message ("Failed to read tracking id %s: %s",
                         priv->tracking_id_path,
                         local_error->message);
              return FALSE;
            }
        }
      else if (g_error_matches (local_error,
                                EMER_ERROR,
                                EMER_ERROR_INVALID_MACHINE_ID))
        {
          g_message ("Failed to read tracking id %s: %s",
                     priv->tracking_id_path,
                     local_error->message);
          return FALSE;
        }
      else
        {
          g_message ("Error occured while reading tracking id at %s: %s",
                     priv->tracking_id_path, local_error->message);
          return FALSE;
        }
    }

  uuid_copy (priv->id, id);

  return TRUE;
}

/*
 * emer_machine_id_provider_get_id:
 * @self: the machine ID provider
 * @machine_id_hex: out location for machine-id stored on disk in hex
 * @uuid: (out caller-allocates) (array fixed-size=16) (element-type guchar):
 * allocated 16-byte return location for a UUID.
 *
 * Retrieves an ID (in the form of a UUID) that is unique to this machine, for
 * use in anonymously identifying metrics data from one of the machine-id
 * provider paths, in priority order. If a file does not exist in a higher
 * priority path, a lower priority path will be tried.
 *
 * Returns: a boolean indicating success or failure of retrieval.
 * If this returns %FALSE, the UUID cannot be trusted to be valid.
 */
gboolean
emer_machine_id_provider_get_id (EmerMachineIdProvider *self,
                                 gchar                **machine_id_hex,
                                 uuid_t                 machine_id)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  if (!priv->id_is_valid)
    {
      if (read_machine_id (self, machine_id_hex))
        {
          priv->id_is_valid = TRUE;
        }
      else
        {
          return FALSE;
        }
    }

  uuid_copy (machine_id, priv->id);
  return TRUE;
}

static gboolean
write_tracking_id_file (const gchar  *path,
                        GError      **error)
{
  uuid_t override_machine_id;
  gchar serialized_override_machine_id[UUID_SERIALIZED_LEN];
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GFile) directory = g_file_get_parent (file);
  g_autoptr(GError) local_error = NULL;
  g_autofree gchar *dehyphenated_serialized_machine_id = NULL;

  uuid_clear (override_machine_id);
  uuid_generate (override_machine_id);
  uuid_unparse (override_machine_id, serialized_override_machine_id);

  dehyphenated_serialized_machine_id = dehyphenate_uuid (serialized_override_machine_id);

  if (!g_file_make_directory_with_parents (directory, NULL, &local_error))
    {
      if (!g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  if (!g_file_set_contents (path, dehyphenated_serialized_machine_id, -1, error))
    return FALSE;

  return TRUE;
}

gboolean
emer_machine_id_provider_reset_tracking_id (EmerMachineIdProvider  *self,
                                            GError                **error)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  if (!write_tracking_id_file (priv->tracking_id_path, error))
    return FALSE;

  g_message ("EmerMachineIdProvider: Will reload from: %s", priv->tracking_id_path);

  priv->id_is_valid = FALSE;
  return TRUE;
}
