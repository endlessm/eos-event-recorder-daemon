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

#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <polkit/polkit.h>

#include "emer-daemon.h"
#include "emer-event-recorder-server.h"
#include "shared/metrics-util.h"

typedef struct _DBusCallbackData
{
  EmerEventRecorderServer *server;
  GDBusMethodInvocation *invocation;
} DBusCallbackData;

static gboolean
on_record_singular_event (EmerEventRecorderServer *server,
                          GDBusMethodInvocation   *invocation,
                          guint32                  user_id,
                          GVariant                *event_id,
                          gint64                   relative_timestamp,
                          gboolean                 has_payload,
                          GVariant                *payload,
                          EmerDaemon              *daemon)
{
  emer_daemon_record_singular_event (daemon, user_id, event_id,
                                     relative_timestamp, has_payload, payload);
  emer_event_recorder_server_complete_record_singular_event (server,
                                                             invocation);
  return TRUE;
}

static gboolean
on_record_aggregate_event (EmerEventRecorderServer *server,
                           GDBusMethodInvocation   *invocation,
                           guint32                  user_id,
                           GVariant                *event_id,
                           gint64                   count,
                           gint64                   relative_timestamp,
                           gboolean                 has_payload,
                           GVariant                *payload,
                           EmerDaemon              *daemon)
{
  emer_daemon_record_aggregate_event (daemon, user_id, event_id, count,
                                      relative_timestamp, has_payload, payload);
  emer_event_recorder_server_complete_record_aggregate_event (server,
                                                              invocation);
  return TRUE;
}

static gboolean
on_record_event_sequence (EmerEventRecorderServer *server,
                          GDBusMethodInvocation   *invocation,
                          guint32                  user_id,
                          GVariant                *event_id,
                          GVariant                *events,
                          EmerDaemon              *daemon)
{
  emer_daemon_record_event_sequence (daemon, user_id, event_id, events);
  emer_event_recorder_server_complete_record_event_sequence (server,
                                                             invocation);
  return TRUE;
}

static gboolean
on_set_enabled (EmerEventRecorderServer *server,
                GDBusMethodInvocation   *invocation,
                gboolean                 enabled,
                EmerDaemon              *daemon)
{
  emer_event_recorder_server_set_enabled (server, enabled);
  emer_event_recorder_server_complete_set_enabled (server, invocation);
  return TRUE;
}

static void
handle_upload_finished (EmerDaemon       *daemon,
                        GAsyncResult     *result,
                        DBusCallbackData *callback_data)
{
  GError *error = NULL;
  gboolean upload_succeeded =
    emer_daemon_upload_events_finish (daemon, result, &error);
  if (upload_succeeded)
    emer_event_recorder_server_complete_upload_events (callback_data->server,
                                                       callback_data->invocation);
  else
    g_dbus_method_invocation_take_error (callback_data->invocation, error);

  g_object_unref (callback_data->server);
  g_free (callback_data);
}

static gboolean
on_upload_events (EmerEventRecorderServer *server,
                  GDBusMethodInvocation   *invocation,
                  EmerDaemon              *daemon)
{
  DBusCallbackData *callback_data = g_new (DBusCallbackData, 1);
  callback_data->server = g_object_ref (server);
  callback_data->invocation = g_object_ref (invocation);
  emer_daemon_upload_events (daemon,
                             (GAsyncReadyCallback) handle_upload_finished,
                             callback_data);
  return TRUE;
}

/*
 * This handler is run in a separate thread, so all operations can be
 * synchronous.
 */
static gboolean
on_authorize_method_check (GDBusInterfaceSkeleton *interface,
                           GDBusMethodInvocation  *invocation,
                           EmerDaemon             *daemon)
{
  const gchar *method_name =
    g_dbus_method_invocation_get_method_name (invocation);
  if (strcmp (method_name, "SetEnabled") != 0)
    return TRUE;

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
                                               "com.endlessm.Metrics.SetEnabled",
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
                                             "Disabling metrics is only "
                                             "allowed from system settings");
    }

  g_object_unref (result);
  return authorized;
}

static gboolean
quit_main_loop (GMainLoop *main_loop)
{
  g_main_loop_quit (main_loop);
  return G_SOURCE_REMOVE;
}

/*
 * Called when a reference to the system bus is acquired. This is where you are
 * supposed to export your well-known name, not in name_acquired; that is too
 * late.
 */
static void
on_bus_acquired (GDBusConnection *system_bus,
                 const gchar     *name,
                 EmerDaemon      *daemon)
{
  EmerEventRecorderServer *server = emer_event_recorder_server_skeleton_new ();

  g_signal_connect (server, "handle-record-singular-event",
                    G_CALLBACK (on_record_singular_event), daemon);
  g_signal_connect (server, "handle-record-aggregate-event",
                    G_CALLBACK (on_record_aggregate_event), daemon);
  g_signal_connect (server, "handle-record-event-sequence",
                    G_CALLBACK (on_record_event_sequence), daemon);
  g_signal_connect (server, "handle-set-enabled",
                    G_CALLBACK (on_set_enabled), daemon);
  g_signal_connect (server, "handle-upload-events",
                    G_CALLBACK (on_upload_events), daemon);
  g_signal_connect (server, "g-authorize-method",
                    G_CALLBACK (on_authorize_method_check), daemon);

  EmerPermissionsProvider *permissions =
    emer_daemon_get_permissions_provider (daemon);
  g_object_bind_property (permissions, "daemon-enabled", server, "enabled",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

  GError *error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (server),
                                         system_bus,
                                         "/com/endlessm/Metrics",
                                         &error))
    {
      g_error ("Could not export metrics interface on system bus: %s.",
               error->message);
    }
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
    {
      g_error ("Could not get connection to system bus.");
    }
  g_error ("Could not acquire name '%s' on system bus.", name);
}

static EmerDaemon *
make_daemon (gint                argc,
             const gchar * const argv[])
{
  gchar *persistent_cache_directory = NULL;
  GOptionEntry option_entries[] =
  {
    { "persistent-cache-directory", 'p', G_OPTION_FLAG_NONE,
      G_OPTION_ARG_FILENAME, &persistent_cache_directory,
      "Store persistent cache at path", "path"},
    { NULL }
  };

  GOptionContext *option_context =
    g_option_context_new (NULL /* parameter string */);
  g_option_context_add_main_entries (option_context, option_entries,
                                     NULL /* translation domain */);

  GError *error = NULL;
  gboolean parse_succeeded =
    g_option_context_parse (option_context, &argc, (gchar ***) &argv, &error);
  g_option_context_free (option_context);

  if (!parse_succeeded)
    {
      g_warning ("Option parsing failed: %s.", error->message);
      g_error_free (error);
      return NULL;
    }

  if (persistent_cache_directory == NULL)
    return emer_daemon_new (PERSISTENT_CACHE_DIR);

  EmerDaemon *daemon = emer_daemon_new (persistent_cache_directory);
  g_free (persistent_cache_directory);

  return daemon;
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  EmerDaemon *daemon = make_daemon (argc, argv);
  if (daemon == NULL)
    return EXIT_FAILURE;

  GMainLoop *main_loop = g_main_loop_new (NULL, TRUE);

  // Shut down on any of these signals.
  g_unix_signal_add (SIGHUP, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGINT, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGTERM, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGUSR1, (GSourceFunc) quit_main_loop, main_loop);
  g_unix_signal_add (SIGUSR2, (GSourceFunc) quit_main_loop, main_loop);

  guint name_id = g_bus_own_name (G_BUS_TYPE_SYSTEM, "com.endlessm.Metrics",
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  (GBusAcquiredCallback) on_bus_acquired,
                                  NULL /* name_acquired_callback */,
                                  (GBusNameLostCallback) on_name_lost,
                                  daemon, NULL /* user data free func */);

  g_main_loop_run (main_loop);

  g_object_unref (daemon);
  g_bus_unown_name (name_id);
  g_main_loop_unref (main_loop);

  return EXIT_SUCCESS;
}
