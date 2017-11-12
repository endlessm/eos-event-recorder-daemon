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
  GStrv paths;
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
 * Filepath at which the random UUID that persistently identifies this machine
 * is stored.
 * In order to protect the anonymity of our users, the ID stored in this file
 * must be randomly generated and not traceable back to the user's device.
 * See http://www.freedesktop.org/software/systemd/man/machine-id.html for more
 * details.
 */
#define DEFAULT_MACHINE_ID_FILEPATH "/etc/machine-id"

/*
 * Filepath where an overridden random UUID, separate from /etc/machine-id
 * is stored. The machine-id might be read from this path and used as the
 * tracking ID in cases where we don't want to continue using the machine-id,
 * either on user request or when we enter the demo mode.
 */
#define TRACKING_ID_OVERRIDE SYSCONFDIR "/eos-metrics-event-recorder/machine-id-override"

enum
{
  PROP_0,
  PROP_PATHS,
  NPROPS
};

static GParamSpec *emer_machine_id_provider_props[NPROPS] = { NULL, };

static GStrv
dup_strv (GStrv input)
{
  gsize length = g_strv_length (input);
  GStrv mem = g_new0 (gchar *, length + 1);
  gsize i;

  for (i = 0; i < length; ++i)
    mem[i] = g_strdup (input[i]);

  mem[length] = NULL;
  return mem;
}

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

static const GStrv
get_id_paths (EmerMachineIdProvider *self)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  return priv->paths;
}

static void
set_id_paths (EmerMachineIdProvider *self,
              const GStrv            given_paths)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  priv->paths = dup_strv (given_paths);
}

static void
emer_machine_id_provider_get_property (GObject    *object,
                                       guint       property_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  EmerMachineIdProvider *self = EMER_MACHINE_ID_PROVIDER (object);
  switch (property_id)
    {
    case PROP_PATHS:
      g_value_set_boxed (value, get_id_paths (self));
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
  switch (property_id)
    {
      case PROP_PATHS:
        set_id_paths (self, g_value_get_boxed (value));
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

  g_strfreev (priv->paths);

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
  emer_machine_id_provider_props[PROP_PATHS] =
    g_param_spec_boxed ("paths", "Paths",
                        "The paths in priority order to the files where the unique identifier is stored.",
                        G_TYPE_STRV,
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
 * @machine_id_file_paths: candidate path to a file to read with the
 *                         machien id see #EmerMachineIdProvider:path
 *
 * Testing function for creating a new #EmerMachineIdProvider in the C API.
 * You only need to use this if you are creating a mock ID provider for unit
 * testing.
 *
 * For all normal uses, you should use emer_machine_id_provider_new()
 * instead.
 *
 * Returns: (transfer full): A new #EmerMachineIdProvider.
 * Free with g_object_unref() when done if using C.
 */
EmerMachineIdProvider *
emer_machine_id_provider_new_full (const gchar **machine_id_file_paths)
{
  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER,
                       "paths", machine_id_file_paths,
                       NULL);
}

/*
 * emer_machine_id_provider_new:
 *
 * Gets the ID provider that you should use for obtaining a unique machine ID in
 * production code. Uses a default filepath.
 *
 * Returns: (transfer full): a production #EmerMachineIdProvider.
 */
EmerMachineIdProvider *
emer_machine_id_provider_new (void)
{
  const gchar *default_paths[] = {
    TRACKING_ID_OVERRIDE,
    DEFAULT_MACHINE_ID_FILEPATH,
    NULL
  };

  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER,
                       "paths", default_paths,
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

static gboolean
read_one_machine_id (const gchar  *machine_id_path,
                     uuid_t        id,
                     GError      **error)
{
  gchar *machine_id_sans_hyphens;
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
  g_free (machine_id_sans_hyphens);

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

  return TRUE;
}

static gboolean
read_machine_id (EmerMachineIdProvider *self)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);
  GStrv iter;
  uuid_t id;

  uuid_clear (id);

  for (iter = priv->paths; *iter != NULL; ++iter)
    {
      g_autoptr(GError) local_error = NULL;

      if (!read_one_machine_id (*iter, id, &local_error))
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_debug ("Machine ID file %s does not exist, tring the next one.",
                       *iter);
            }
          else if (g_error_matches (local_error,
                                    EMER_ERROR,
                                    EMER_ERROR_INVALID_MACHINE_ID))
            {
              g_critical ("Failed to read machine id %s: %s",
                          *iter,
                          local_error->message);
            }
          continue;
        }

      break;
    }

  if (uuid_is_null (id))
    {
      g_critical ("Failed to read in a unique machine id");
      return FALSE;
    }

  uuid_copy (priv->id, id);
  uuid_clear (id);

  return TRUE;
}

/*
 * emer_machine_id_provider_get_id:
 * @self: the machine ID provider
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
                                 uuid_t                 machine_id)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  if (!priv->id_is_valid)
    {
      if (read_machine_id (self))
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

void
emer_machine_id_provider_reload (EmerMachineIdProvider *self)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);
  g_autofree gchar *formatted_paths = g_strjoinv ("\n", priv->paths);

  g_message ("EmerMachineIdProvider: Will reload from:\n%s", formatted_paths);

  priv->id_is_valid = FALSE;
}
