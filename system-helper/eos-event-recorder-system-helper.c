/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 - 2017 Endless Mobile, Inc. */

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

#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>
#include <polkit/polkit.h>

#include <uuid/uuid.h>

#include <shared/metrics-util.h>

#include "emer-metrics-system-helper.h"

#define TRACKING_ID_OVERRIDE SYSCONFDIR "/eos-metrics-event-recorder/machine-id-override"

static gboolean
on_reset_tracking_id (EmerMetricsSystemHelper *interface,
                      GDBusMethodInvocation   *invocation,
                      gpointer                 user_data G_GNUC_UNUSED)
{
  g_autoptr(GError) error = NULL;

  if (!write_tracking_id_file (TRACKING_ID_OVERRIDE, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  emer_metrics_system_helper_complete_reset_tracking_id (interface,
                                                         invocation);

  return TRUE;
}

/*
 * This handler is run in a separate thread, so all operations can be
 * synchronous.
 */
static gboolean
on_authorize_method_check (GDBusInterfaceSkeleton *interface,
                           GDBusMethodInvocation  *invocation,
                           gpointer                user_data G_GNUC_UNUSED)
{
  const gchar *method_name =
    g_dbus_method_invocation_get_method_name (invocation);

  if (g_strcmp0 (method_name, "ResetTrackingId") != 0)
    return FALSE;

  GError *error = NULL;
  PolkitAuthority *authority =
    polkit_authority_get_sync (NULL /*GCancellable*/, &error);
  if (authority == NULL)
    {
      g_critical ("Could not get PolicyKit authority: %s.", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      return FALSE;
    }

  const gchar *sender_name = g_dbus_method_invocation_get_sender (invocation);
  PolkitSubject *subject = polkit_system_bus_name_new (sender_name);

  PolkitAuthorizationResult *result =
    polkit_authority_check_authorization_sync (authority,
                                               subject,
                                               "com.endlessm.MetricsSystemHelper.ResetTrackingId",
                                               NULL /*PolkitDetails*/,
                                               POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE,
                                               NULL /*GCancellable*/,
                                               &error);
  g_object_unref (authority);
  g_object_unref (subject);
  if (result == NULL)
    {
      g_critical ("Could not get PolicyKit authorization result: %s.",
                  error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      return FALSE;
    }

  gboolean authorized = polkit_authorization_result_get_is_authorized (result);
  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_AUTH_FAILED,
                                             "Not authorized to perform: %s",
                                             method_name);
    }

  g_object_unref (result);
  return authorized;
}

/*
 * Called when a reference to the system bus is acquired. This is where you are
 * supposed to export your well-known name, not in name_acquired; that is too
 * late.
 */
static void
on_bus_acquired (GDBusConnection *system_bus,
                 const gchar     *name,
                 gpointer         user_data G_GNUC_UNUSED)
{
  EmerMetricsSystemHelper *helper = emer_metrics_system_helper_skeleton_new ();

  g_signal_connect (helper, "handle-reset-tracking-id",
                    G_CALLBACK (on_reset_tracking_id), NULL);
  g_signal_connect (helper, "g-authorize-method",
                    G_CALLBACK (on_authorize_method_check), NULL);

  GError *error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         system_bus,
                                         "/com/endlessm/MetricsSystemHelper",
                                         &error))
    g_error ("Could not export metrics interface on system bus: %s.",
             error->message);
}

/*
 * This is called if ownership of the well-known name is lost. Since this
 * service doesn't own and un-own the name during its lifetime, this is only
 * called if there is an error acquiring it in the first place.
 */
static void
on_name_lost (GDBusConnection *system_bus,
              const gchar     *name)
{
  /*
   * This handler is called with a NULL connection if the bus could not be
   * acquired.
   */
  if (system_bus == NULL)
    g_error ("Could not get connection to system bus.");

  g_error ("Could not acquire name '%s' on system bus.", name);
}

static gboolean
quit_main_loop (GMainLoop *main_loop)
{
  g_main_loop_quit (main_loop);
  return G_SOURCE_REMOVE;
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  GMainLoop *main_loop = g_main_loop_new (NULL, TRUE);

  // Shut down on any of these signals.
  g_unix_signal_add (SIGHUP, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGINT, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGTERM, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGUSR1, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGUSR2, (GSourceFunc) quit_main_loop, main_loop);

  guint name_id = g_bus_own_name (G_BUS_TYPE_SYSTEM, "com.endlessm.MetricsSystemHelper",
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  (GBusAcquiredCallback) on_bus_acquired,
                                  NULL /* name_acquired_callback */,
                                  (GBusNameLostCallback) on_name_lost,
                                  daemon, NULL /* user data free func */);

  g_main_loop_run (main_loop);

  g_bus_unown_name (name_id);
  g_main_loop_unref (main_loop);

  return EXIT_SUCCESS;
}
