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

#include "config.h"
#include "emer-machine-id-provider.h"

#include <uuid/uuid.h>

#include <gio/gio.h>
#include <glib.h>

#define MACHINE_ID "387c5206-24b5-4513-a34f-72689d5c0a0e"
#define OVERRIDE_MACHINE_ID "67523704-f885-49ea-9680-450782c9dd66"

typedef struct EmerMachineIdProviderPrivate
{
  gboolean has_override;
} EmerMachineIdProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerMachineIdProvider, emer_machine_id_provider, G_TYPE_OBJECT)

static void
emer_machine_id_provider_class_init (EmerMachineIdProviderClass *klass)
{
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

gboolean
emer_machine_id_provider_get_id (EmerMachineIdProvider *self,
                                 gchar                **machine_id_hex,
                                 uuid_t                 machine_id)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);

  /* Try to read the override file first if we have one */
  if (priv->has_override)
    {
      g_assert_cmpint (uuid_parse (OVERRIDE_MACHINE_ID, machine_id), ==, 0);
      return TRUE;
    }

  g_assert_cmpint (uuid_parse (MACHINE_ID, machine_id), ==, 0);
  return TRUE;
}

gboolean
emer_machine_id_provider_reset_tracking_id (EmerMachineIdProvider  *self,
                                            GError                **error)
{
  EmerMachineIdProviderPrivate *priv =
    emer_machine_id_provider_get_instance_private (self);
  priv->has_override = TRUE;
  return TRUE;
}
