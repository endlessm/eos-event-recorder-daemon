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

#include "emer-permissions-provider.h"

#include <string.h>

#include <gio/gio.h>
#include <glib.h>

#define SIGNAL_TIMEOUT_SEC 5

/*
 * The maximum number of calls to g_main_loop_run in any single test that aren't
 * guaranteed to be paired with calls to g_main_loop_quit.
 */
#define MAX_NUM_TIMEOUTS 2

#define PERMISSIONS_CONFIG_FILE_ENABLED_TEST \
  "[global]\n" \
  "enabled=true\n" \
  "uploading_enabled=true\n" \
  "environment=test"
#define PERMISSIONS_CONFIG_FILE_DISABLED_TEST \
  "[global]\n" \
  "enabled=false\n" \
  "uploading_enabled=true\n" \
  "environment=test"
#define PERMISSIONS_CONFIG_FILE_UPLOADING_DISABLED_TEST \
  "[global]\n" \
  "enabled=true\n" \
  "uploading_enabled=false\n" \
  "environment=test"
#define PERMISSIONS_CONFIG_FILE_INVALID \
  "lavubeu;f'w943ty[jdn;fbl\n"
#define PERMISSIONS_CONFIG_FILE_ENABLED_DEV \
  "[global]\n" \
  "enabled=true\n" \
  "uploading_enabled=true\n" \
  "environment=dev"
#define PERMISSIONS_CONFIG_FILE_ENABLED_PRODUCTION \
  "[global]\n" \
  "enabled=true\n" \
  "uploading_enabled=true\n" \
  "environment=production"
#define PERMISSIONS_CONFIG_FILE_ENABLED_INVALID_ENVIRONMENT \
  "[global]\n" \
  "enabled=true\n" \
  "uploading_enabled=true\n" \
  "environment=invalid"
#define OSTREE_CONFIG_FILE_STAGING_URL \
  "[core]\n" \
  "repo_version=1\n" \
  "mode=bare\n\n" \
  "[remote \"eos\"]\n" \
  "url=http://fakeurl.with/staging/in/path\n" \
  "branches=master/i386;"
#define OSTREE_CONFIG_FILE_NON_STAGING_URL \
  "[core]\n" \
  "repo_version=1\n" \
  "mode=bare\n\n" \
  "[remote \"eos\"]\n" \
  "url=http://fakeurl.without/term/in/path\n" \
  "branches=master/i386;"

typedef struct {
  GFile *permissions_config_file;
  GFile *ostree_config_file;
  EmerPermissionsProvider *test_object;

  GMainLoop *main_loop; /* for asynchronous tests */
  guint failsafe_source_id;
  gint num_timeouts;

  gboolean notify_daemon_called;
  gboolean notify_daemon_called_with;
} Fixture;

/* Callback for notify::daemon-enabled that records what it was called with and
quits the main loop so the test can continue. */
static void
on_notify_daemon_enabled (EmerPermissionsProvider *permissions_provider,
                          GParamSpec              *pspec,
                          Fixture                 *fixture)
{
  if (!fixture->notify_daemon_called)
    fixture->notify_daemon_called_with =
      emer_permissions_provider_get_daemon_enabled (permissions_provider);
  fixture->notify_daemon_called = TRUE;
  g_main_loop_quit (fixture->main_loop);
}

static gboolean
quit_main_loop (Fixture *fixture)
{
  if (g_main_loop_is_running (fixture->main_loop))
    g_main_loop_quit (fixture->main_loop);

  return ++fixture->num_timeouts < MAX_NUM_TIMEOUTS ?
    G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gchar *
write_config_file (GFileIOStream *stream,
                   const gchar   *contents,
                   GFile         *config_file)
{
  GOutputStream *ostream = g_io_stream_get_output_stream (G_IO_STREAM (stream));

  if (contents != NULL)
    {
      gboolean write_succeeded =
        g_output_stream_write_all (ostream, contents, strlen (contents),
                                   NULL /* bytes written */, NULL, NULL);
      g_assert_true (write_succeeded);
      g_object_unref (stream);
    }
  else
    {
      g_object_unref (stream);
      g_assert_true (g_file_delete (config_file, NULL, NULL));
    }

  return g_file_get_path (config_file);
}

static void
setup_config_files (Fixture     *fixture,
                    const gchar *permissions_config_file_contents,
                    const gchar *ostree_config_file_contents)
{
  GFileIOStream *stream = NULL;
  fixture->permissions_config_file =
    g_file_new_tmp ("test-permissions-providerXXXXXX", &stream, NULL);
  g_assert_nonnull (fixture->permissions_config_file);

  gchar *permissions_config_file_path =
    write_config_file (stream,
                       permissions_config_file_contents,
                       fixture->permissions_config_file);

  fixture->ostree_config_file =
    g_file_new_tmp ("test-permissions-providerXXXXXX", &stream, NULL);
  g_assert_nonnull (fixture->ostree_config_file);

  gchar *ostree_config_file_path =
    write_config_file (stream,
                       ostree_config_file_contents,
                       fixture->ostree_config_file);
  fixture->test_object =
    emer_permissions_provider_new_full (permissions_config_file_path,
                                        ostree_config_file_path);
  g_free (permissions_config_file_path);
  g_free (ostree_config_file_path);

  fixture->main_loop = g_main_loop_new (NULL, FALSE);
  fixture->notify_daemon_called = FALSE;
  g_signal_connect (fixture->test_object, "notify::daemon-enabled",
                    G_CALLBACK (on_notify_daemon_enabled), fixture);

  /* Failsafe: stop waiting for signals in async tests after
     SIGNAL_TIMEOUT_SEC seconds. */
  fixture->failsafe_source_id =
    g_timeout_add_seconds (SIGNAL_TIMEOUT_SEC, (GSourceFunc) quit_main_loop,
                           fixture);
}

/* Pass NULL to permissions_config_file_contents if you don't want to create a
file on disk. A filename will be created in any case and passed to the object's
constructor. */
static void
setup_with_config_file (Fixture       *fixture,
                        gconstpointer  permissions_config_file_contents)
{
  setup_config_files (fixture,
                      (const gchar *) permissions_config_file_contents,
                      (const gchar *) OSTREE_CONFIG_FILE_NON_STAGING_URL);
}

/* Pass NULL to ostree_config_file_contents if you don't want to create a file
on disk. A filename will be created in any case and passed to the object's
constructor. */
static void
setup_dev_environment_with_ostree_file (Fixture       *fixture,
                                        gconstpointer  ostree_config_file_contents)
{
  setup_config_files (fixture,
                      (const gchar *) PERMISSIONS_CONFIG_FILE_ENABLED_DEV,
                      (const gchar *) ostree_config_file_contents);
}

/* Pass NULL to ostree_config_file_contents if you don't want to create a file
on disk. A filename will be created in any case and passed to the object's
constructor. */
static void
setup_production_environment_with_ostree_file (Fixture       *fixture,
                                               gconstpointer  ostree_config_file_contents)
{
  const gchar *permissions_config_file_contents =
    (const gchar *) PERMISSIONS_CONFIG_FILE_ENABLED_PRODUCTION;

  setup_config_files (fixture,
                      permissions_config_file_contents,
                      (const gchar *) ostree_config_file_contents);
}

static void
setup_invalid_file (Fixture      *fixture,
                    gconstpointer permissions_config_file_contents)
{
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
                         "*Permissions config file*was invalid or could not be "
                         "read. Loading fallback data*");
  setup_with_config_file (fixture, permissions_config_file_contents);
}

static void
setup_invalid_environment (Fixture      *fixture,
                           gconstpointer permissions_config_file_contents)
{
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "*Error: Metrics environment is set to: * in *. "
                         "Valid metrics environments are: dev, test, "
                         "production.");
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "Metrics environment was not present or was invalid. "
                         "Assuming 'test' environment.");
  setup_with_config_file (fixture, permissions_config_file_contents);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_source_remove (fixture->failsafe_source_id);

  /* Might not exist. */
  g_file_delete (fixture->permissions_config_file, NULL, NULL);

  g_clear_object (&fixture->permissions_config_file);

  /* Might not exist. */
  g_file_delete (fixture->ostree_config_file, NULL, NULL);

  g_clear_object (&fixture->ostree_config_file);
  g_clear_object (&fixture->test_object);
  g_main_loop_unref (fixture->main_loop);
}

/* This test is run several times with different data; once with a config file,
once with NULL. */
static void
test_permissions_provider_new (Fixture      *fixture,
                               gconstpointer unused)
{
  g_assert_nonnull (fixture->test_object);
}

static void
test_permissions_provider_new_invalid_file (Fixture      *fixture,
                                            gconstpointer unused)
{
  g_assert_nonnull (fixture->test_object);
  g_test_assert_expected_messages ();
}

static void
test_permissions_provider_get_daemon_enabled (Fixture      *fixture,
                                              gconstpointer unused)
{
  gboolean daemon_enabled =
    emer_permissions_provider_get_daemon_enabled (fixture->test_object);
  g_assert_true (daemon_enabled);
}

static void
test_permissions_provider_get_daemon_enabled_false (Fixture      *fixture,
                                                    gconstpointer unused)
{
  gboolean daemon_enabled =
    emer_permissions_provider_get_daemon_enabled (fixture->test_object);
  g_assert_false (daemon_enabled);
}

static void
test_permissions_provider_get_daemon_enabled_fallback (Fixture      *fixture,
                                                       gconstpointer unused)
{
  gboolean daemon_enabled =
    emer_permissions_provider_get_daemon_enabled (fixture->test_object);
  g_assert_false (daemon_enabled);
  g_test_assert_expected_messages ();
}

static void
test_permissions_provider_get_uploading_enabled (Fixture      *fixture,
                                                 gconstpointer unused)
{
  gboolean uploading_enabled =
    emer_permissions_provider_get_uploading_enabled (fixture->test_object);
  g_assert_true (uploading_enabled);
}

static void
test_permissions_provider_get_uploading_enabled_false (Fixture      *fixture,
                                                       gconstpointer unused)
{
  gboolean uploading_enabled =
    emer_permissions_provider_get_uploading_enabled (fixture->test_object);
  g_assert_false (uploading_enabled);
}

static void
test_permissions_provider_get_uploading_enabled_fallback (Fixture      *fixture,
                                                          gconstpointer unused)
{
  gboolean uploading_enabled =
    emer_permissions_provider_get_uploading_enabled (fixture->test_object);
  g_assert_true (uploading_enabled);
  g_test_assert_expected_messages ();
}

static void
test_permissions_provider_get_environment_test (Fixture      *fixture,
                                                gconstpointer unused)
{
  gchar *environment =
    emer_permissions_provider_get_environment (fixture->test_object);
  g_assert_cmpstr (environment, ==, "test");
  g_clear_pointer (&environment, g_free);
}

static void
test_permissions_provider_get_environment_test_fallback (Fixture      *fixture,
                                                         gconstpointer unused)
{
  gchar *environment =
    emer_permissions_provider_get_environment (fixture->test_object);
  g_assert_cmpstr (environment, ==, "test");
  g_clear_pointer (&environment, g_free);
  g_test_assert_expected_messages ();
}

static void
test_permissions_provider_get_environment_dev (Fixture      *fixture,
                                               gconstpointer unused)
{
  gchar *environment =
    emer_permissions_provider_get_environment (fixture->test_object);
  g_assert_cmpstr (environment, ==, "dev");
  g_clear_pointer (&environment, g_free);
}

static void
test_permissions_provider_get_environment_production (Fixture      *fixture,
                                                      gconstpointer unused)
{
  gchar *environment =
    emer_permissions_provider_get_environment (fixture->test_object);
  g_assert_cmpstr (environment, ==, "production");
  g_clear_pointer (&environment, g_free);
}

/* This is run in an idle function so that it doesn't quit the main loop before
the main loop has started. */
static gboolean
set_daemon_enabled_false_idle (Fixture *fixture)
{
  emer_permissions_provider_set_daemon_enabled (fixture->test_object, FALSE);
  return G_SOURCE_REMOVE;
}

static void
test_permissions_provider_set_daemon_enabled (Fixture      *fixture,
                                              gconstpointer unused)
{
  g_idle_add ((GSourceFunc) set_daemon_enabled_false_idle, fixture);
  g_main_loop_run (fixture->main_loop);
  g_assert_true (fixture->notify_daemon_called);
  g_assert_false (fixture->notify_daemon_called_with);
}

/* Callback for file monitor change, which quits the main loop. */
static void
on_config_file_changed (GFileMonitor     *monitor,
                        GFile            *file,
                        GFile            *other_file,
                        GFileMonitorEvent event_type,
                        Fixture          *fixture)
{
  if (event_type == G_FILE_MONITOR_EVENT_CREATED ||
      event_type == G_FILE_MONITOR_EVENT_CHANGED)
    g_main_loop_quit (fixture->main_loop);
}

static void
test_permissions_provider_set_daemon_enabled_updates_config_file (Fixture      *fixture,
                                                                  gconstpointer unused)
{
  gchar *contents;
  gboolean loaded_file =
    g_file_load_contents (fixture->permissions_config_file, NULL, &contents,
                          NULL, NULL, NULL);
  g_assert_true (loaded_file);
  g_assert_nonnull (strstr (contents, "enabled=true"));
  g_free (contents);

  g_idle_add ((GSourceFunc) set_daemon_enabled_false_idle, fixture);

  GFileMonitor *monitor =
    g_file_monitor_file (fixture->permissions_config_file, G_FILE_MONITOR_NONE,
                         NULL, NULL);
  g_assert_nonnull (monitor);

  g_signal_connect (monitor, "changed", G_CALLBACK (on_config_file_changed),
                    fixture);

  /* Run the main loop twice, to wait for two events: the property notification
  and a "changed" or "created" file monitor event. */
  g_main_loop_run (fixture->main_loop);
  g_main_loop_run (fixture->main_loop);

  g_object_unref (monitor);

  loaded_file =
    g_file_load_contents (fixture->permissions_config_file, NULL, &contents,
                          NULL, NULL, NULL);
  g_assert_true (loaded_file);
  g_assert_nonnull (strstr (contents, "enabled=false"));
  g_free (contents);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);

#define ADD_PERMISSIONS_PROVIDER_TEST(path, file_contents, setup, test_func) \
  g_test_add ((path), Fixture, (file_contents), (setup), (test_func), teardown);

  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/new/valid-file",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_new);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/new/absent-file",
                                 NULL, setup_with_config_file,
                                 test_permissions_provider_new);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/new/invalid-file",
                                 PERMISSIONS_CONFIG_FILE_INVALID,
                                 setup_invalid_file,
                                 test_permissions_provider_new_invalid_file);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-daemon-enabled/enabled",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_get_daemon_enabled);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-daemon-enabled/disabled",
                                 PERMISSIONS_CONFIG_FILE_DISABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_get_daemon_enabled_false);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-daemon-enabled/invalid-file",
                                 PERMISSIONS_CONFIG_FILE_INVALID,
                                 setup_invalid_file,
                                 test_permissions_provider_get_daemon_enabled_fallback);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-uploading-enabled/enabled",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_get_uploading_enabled);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-uploading-enabled/disabled",
                                 PERMISSIONS_CONFIG_FILE_UPLOADING_DISABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_get_uploading_enabled_false);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-uploading-enabled/invalid-file",
                                 PERMISSIONS_CONFIG_FILE_INVALID,
                                 setup_invalid_file,
                                 test_permissions_provider_get_uploading_enabled_fallback);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-environment/test",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_get_environment_test);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-environment/dev",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_DEV,
                                 setup_with_config_file,
                                 test_permissions_provider_get_environment_dev);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-environment/production",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_PRODUCTION,
                                 setup_with_config_file,
                                 test_permissions_provider_get_environment_production);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-environment/invalid-environment",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_INVALID_ENVIRONMENT,
                                 setup_invalid_environment,
                                 test_permissions_provider_get_environment_test_fallback);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-environment/production-staging",
                                 OSTREE_CONFIG_FILE_STAGING_URL,
                                 setup_production_environment_with_ostree_file,
                                 test_permissions_provider_get_environment_dev);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-environment/production-non-staging",
                                 OSTREE_CONFIG_FILE_NON_STAGING_URL,
                                 setup_production_environment_with_ostree_file,
                                 test_permissions_provider_get_environment_production);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-environment/dev-non-staging",
                                 OSTREE_CONFIG_FILE_NON_STAGING_URL,
                                 setup_dev_environment_with_ostree_file,
                                 test_permissions_provider_get_environment_dev);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/set-daemon-enabled",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_set_daemon_enabled);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/set-daemon-enabled-updates-config-file",
                                 PERMISSIONS_CONFIG_FILE_ENABLED_TEST,
                                 setup_with_config_file,
                                 test_permissions_provider_set_daemon_enabled_updates_config_file);

#undef ADD_PERMISSIONS_PROVIDER_TEST

  return g_test_run ();
}
