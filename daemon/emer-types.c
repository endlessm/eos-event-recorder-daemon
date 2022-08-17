/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2017 Endless Mobile, Inc. */

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
#include "emer-types.h"

#include <gio/gio.h>

#define EMER_ERROR_DOMAIN "com.endlessm.Metrics.Error"

static const GDBusErrorEntry emer_error_entries[] = {
    { EMER_ERROR_METRICS_DISABLED, EMER_ERROR_DOMAIN ".MetricsDisabled" },
    { EMER_ERROR_UPLOADING_DISABLED, EMER_ERROR_DOMAIN ".UploadingDisabled" },
    { EMER_ERROR_INVALID_MACHINE_ID, EMER_ERROR_DOMAIN ".InvalidMachineId" },
    { EMER_ERROR_INVALID_EVENT_ID, EMER_ERROR_DOMAIN ".InvalidEventId" },
};

G_STATIC_ASSERT (G_N_ELEMENTS (emer_error_entries) == EMER_ERROR_LAST + 1);

GQuark
emer_error_quark (void)
{
  static volatile gsize quark_volatile = 0;
  g_dbus_error_register_error_domain ("emer-error-quark",
                                      &quark_volatile,
                                      emer_error_entries,
                                      G_N_ELEMENTS (emer_error_entries));
  return (GQuark) quark_volatile;
}
