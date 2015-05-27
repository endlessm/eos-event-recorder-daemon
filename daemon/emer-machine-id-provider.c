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

#include "emer-machine-id-provider.h"

#include "shared/metrics-util.h"

#include <string.h>
#include <uuid/uuid.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>

typedef struct EmerMachineIdProviderPrivate
{
  gchar *path;
  uuid_t id;
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

enum
{
  PROP_0,
  PROP_PATH,
  NPROPS
};

static GParamSpec *emer_machine_id_provider_props[NPROPS] = { NULL, };

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

static const gchar *
get_id_path (EmerMachineIdProvider *self)
{
  EmerMachineIdProviderPrivate *priv = emer_machine_id_provider_get_instance_private (self);
  return priv->path;
}

static void
set_id_path (EmerMachineIdProvider *self,
             const gchar           *given_path)
{
  EmerMachineIdProviderPrivate *priv = emer_machine_id_provider_get_instance_private (self);
  priv->path = g_strdup (given_path);
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
    case PROP_PATH:
      g_value_set_string (value, get_id_path (self));
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
      case PROP_PATH:
        set_id_path (self, g_value_get_string (value));
        break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_machine_id_provider_finalize (GObject *object)
{
  EmerMachineIdProvider *self = EMER_MACHINE_ID_PROVIDER (object);
  EmerMachineIdProviderPrivate *priv = emer_machine_id_provider_get_instance_private (self);
  g_free (priv->path);

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
  emer_machine_id_provider_props[PROP_PATH] =
    g_param_spec_string ("path", "Path",
                         "The path to the file where the unique identifier is stored.",
                         DEFAULT_MACHINE_ID_FILEPATH,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_machine_id_provider_props);
}

// Mandatory empty function.
static void
emer_machine_id_provider_init (EmerMachineIdProvider *self)
{
}

/*
 * emer_machine_id_provider_new_full:
 * @machine_id_file_path: path to a file; see #EmerMachineIdProvider:path
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
emer_machine_id_provider_new_full (const gchar *machine_id_file_path)
{
  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER,
                       "path", machine_id_file_path,
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
  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER, NULL);
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
read_machine_id (EmerMachineIdProvider *self)
{
  EmerMachineIdProviderPrivate *priv = emer_machine_id_provider_get_instance_private (self);

  gchar *machine_id_sans_hyphens;
  gsize machine_id_sans_hyphens_length;
  GError *error = NULL;
  gboolean read_succeeded =
    g_file_get_contents (priv->path, &machine_id_sans_hyphens,
                         &machine_id_sans_hyphens_length, &error);
  if (!read_succeeded)
    {
      g_critical ("Failed to read machine ID file (%s).", priv->path);
      return FALSE;
    }

  if (strlen (machine_id_sans_hyphens) != machine_id_sans_hyphens_length)
    {
      g_critical ("Machine ID file (%s) contained null byte, but should be "
                  "hexadecimal.",
                  priv->path);
      return FALSE;
    }

  if (machine_id_sans_hyphens_length != FILE_LENGTH)
    {
      g_critical ("Machine ID file (%s) contained %" G_GSIZE_FORMAT " bytes, "
                  "but expected %d bytes.",
                  priv->path, machine_id_sans_hyphens_length,
                  FILE_LENGTH);
      return FALSE;
    }

  gchar *hyphenated_machine_id = hyphenate_uuid (machine_id_sans_hyphens);
  g_free (machine_id_sans_hyphens);

  int parse_failed = uuid_parse (hyphenated_machine_id, priv->id);
  g_free (hyphenated_machine_id);

  if (parse_failed != 0)
    {
      g_critical ("Machine ID file (%s) did not contain UUID.", priv->path);
      return FALSE;
    }

  return TRUE;
}

/*
 * emer_machine_id_provider_get_id:
 * @self: the machine ID provider
 * @uuid: (out caller-allocates) (array fixed-size=16) (element-type guchar):
 * allocated 16-byte return location for a UUID.
 *
 * Retrieves an ID (in the form of a UUID) that is unique to this machine, for
 * use in anonymously identifying metrics data.
 *
 * Returns: a boolean indicating success or failure of retrieval.
 * If this returns %FALSE, the UUID cannot be trusted to be valid.
 */
gboolean
emer_machine_id_provider_get_id (EmerMachineIdProvider *self,
                                 uuid_t                 machine_id)
{
  EmerMachineIdProviderPrivate *priv = emer_machine_id_provider_get_instance_private (self);
  static gboolean id_is_valid = FALSE;
  G_LOCK_DEFINE_STATIC (id_is_valid);

  G_LOCK (id_is_valid);
  if (!id_is_valid)
    {
      if (read_machine_id (self))
        {
          id_is_valid = TRUE;
        }
      else
        {
          G_UNLOCK (id_is_valid);
          return FALSE;
        }
    }
  G_UNLOCK (id_is_valid);

  memcpy(machine_id, priv->id, UUID_LENGTH);
  return TRUE;
}
