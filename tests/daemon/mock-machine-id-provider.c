/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2015 Endless Mobile, Inc. */

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
#include "mock-machine-id-provider.h"

#include <uuid/uuid.h>

#include <gio/gio.h>
#include <glib.h>

#define MACHINE_ID "387c5206-24b5-4513-a34f-72689d5c0a0e"

typedef struct EmerMachineIdProviderPrivate
{
  gchar *override_path;
} EmerMachineIdProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerMachineIdProvider, emer_machine_id_provider, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_OVERRIDE_PATH,
  NPROPS
};

static GParamSpec *emer_machine_id_provider_props[NPROPS] = { NULL, };

static void
emer_machine_id_provider_finalize (GObject *object)
{
  EmerMachineIdProvider *self = EMER_MACHINE_ID_PROVIDER (object);
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  g_free (priv->override_path);

  G_OBJECT_CLASS (emer_machine_id_provider_parent_class)->finalize (object);
}

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
    case PROP_OVERRIDE_PATH:
      g_value_set_string (value, priv->override_path);
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
      case PROP_OVERRIDE_PATH:
        priv->override_path = g_value_dup_string (value);
        break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
emer_machine_id_provider_class_init (EmerMachineIdProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = emer_machine_id_provider_get_property;
  object_class->set_property = emer_machine_id_provider_set_property;
  object_class->finalize = emer_machine_id_provider_finalize;

  /* Blurb string is good enough default documentation for this */
  emer_machine_id_provider_props[PROP_OVERRIDE_PATH] =
    g_param_spec_string ("override-path", "Override path",
                         "File to check first before returning default machine-id.",
                         NULL,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_machine_id_provider_props);
}

static void
emer_machine_id_provider_init (EmerMachineIdProvider *self)
{
}

/* MOCK PUBLIC API */

EmerMachineIdProvider *
emer_machine_id_provider_new (void)
{
  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER, NULL);
}

EmerMachineIdProvider *
emer_machine_id_provider_new_with_override_path (const gchar *machine_id_file_path)
{
  return g_object_new (EMER_TYPE_MACHINE_ID_PROVIDER,
                       "override-path", machine_id_file_path,
                       NULL);
}

gboolean
emer_machine_id_provider_get_id (EmerMachineIdProvider *self,
                                 uuid_t                 machine_id)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  /* Try to read the override file first if we have one */
  if (priv->override_path)
    {
      g_autofree gchar *machine_id_contents = NULL;
      g_autoptr(GError) error = NULL;

      if (!g_file_get_contents (priv->override_path, &machine_id_contents, NULL, &error))
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_clear_error (&error);
              g_assert_cmpint (uuid_parse (MACHINE_ID, machine_id), ==, 0);
              return TRUE;
            }
        }

      g_assert_cmpint (uuid_parse (machine_id_contents, machine_id), ==, 0);
      return TRUE;
    }

  g_assert_cmpint (uuid_parse (MACHINE_ID, machine_id), ==, 0);
  return TRUE;
}

void
emer_machine_id_provider_reload (EmerMachineIdProvider *self)
{
}
