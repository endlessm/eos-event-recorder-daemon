/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-permissions-provider.h"

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#define CONFIG_FILE_ENABLED_CONTENTS \
  "[global]\n" \
  "enabled=true\n"
#define CONFIG_FILE_DISABLED_CONTENTS \
  "[global]\n" \
  "enabled=false\n"
#define CONFIG_FILE_INVALID_CONTENTS "lavubeu;f'w943ty[jdn;fbl\n"

typedef struct {
  GFile *temp_file;
  EmerPermissionsProvider *test_object;

  GMainLoop *main_loop; /* for asynchronous tests */
  guint failsafe_source_id;

  gboolean notify_daemon_called;
  gboolean notify_daemon_called_with;
} Fixture;

/* Callback for notify::daemon-enabled that records what it was called with and
quits the main loop so the test can continue. */
static void
on_notify_daemon_enabled (GObject    *test_object,
                          GParamSpec *pspec,
                          Fixture    *fixture)
{
  if (!fixture->notify_daemon_called)
    fixture->notify_daemon_called_with =
      emer_permissions_provider_get_daemon_enabled (EMER_PERMISSIONS_PROVIDER (test_object));
  fixture->notify_daemon_called = TRUE;
  g_main_loop_quit (fixture->main_loop);
}

/* Pass NULL to config_file_contents if you don't want to create a file on disk.
A file name will be created in any case and passed to the object's constructor.
*/
static void
setup (Fixture      *fixture,
       gconstpointer data)
{
  const gchar *config_file_contents = (const gchar *)data;

  GFileIOStream *stream = NULL;
  fixture->temp_file = g_file_new_tmp ("test-permissions-providerXXXXXX",
                                       &stream, NULL);
  g_assert (fixture->temp_file != NULL);

  GOutputStream *ostream = g_io_stream_get_output_stream (G_IO_STREAM (stream));

  if (config_file_contents != NULL)
    {
      g_assert (g_output_stream_write_all (ostream, config_file_contents,
                                           strlen (config_file_contents),
                                           NULL /* bytes written */,
                                           NULL, NULL));
      g_object_unref (stream);
    }
  else
    {
      g_object_unref (stream);
      g_assert (g_file_delete (fixture->temp_file, NULL, NULL));
    }

  gchar *config_file_path = g_file_get_path (fixture->temp_file);
  fixture->test_object = emer_permissions_provider_new_full (config_file_path);
  g_free (config_file_path);

  fixture->main_loop = g_main_loop_new (NULL, FALSE);
  fixture->notify_daemon_called = FALSE;
  g_signal_connect (fixture->test_object, "notify::daemon-enabled",
                    G_CALLBACK (on_notify_daemon_enabled), fixture);

  /* Failsafe: quit any hung async tests after 5 seconds. */
  fixture->failsafe_source_id = g_timeout_add_seconds (5,
                                                       (GSourceFunc) g_main_loop_quit,
                                                       fixture->main_loop);
}

static void
setup_invalid_file (Fixture      *fixture,
                    gconstpointer contents)
{
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL,
                         "*Permissions config file*was invalid or could not be "
                         "read. Loading fallback data*");
  setup (fixture, contents);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_source_remove (fixture->failsafe_source_id);
  g_file_delete (fixture->temp_file, NULL, NULL); /* might not exist */
  g_clear_object (&fixture->temp_file);
  g_clear_object (&fixture->test_object);
  g_main_loop_unref (fixture->main_loop);
}

/* This test is run several times with different data; once with a config file,
once with NULL. */
static void
test_permissions_provider_new (Fixture      *fixture,
                               gconstpointer unused)
{
  g_assert (fixture->test_object != NULL);
}

static void
test_permissions_provider_new_invalid_file (Fixture      *fixture,
                                            gconstpointer unused)
{
  g_assert (fixture->test_object != NULL);
  g_test_assert_expected_messages ();
}

static void
test_permissions_provider_get_daemon_enabled (Fixture      *fixture,
                                              gconstpointer unused)
{
  g_assert (emer_permissions_provider_get_daemon_enabled (fixture->test_object));
}

static void
test_permissions_provider_get_daemon_enabled_false (Fixture      *fixture,
                                                    gconstpointer unused)
{
  g_assert_false (emer_permissions_provider_get_daemon_enabled (fixture->test_object));
}

static void
test_permissions_provider_get_daemon_enabled_fallback (Fixture      *fixture,
                                                       gconstpointer unused)
{
  g_assert_false (emer_permissions_provider_get_daemon_enabled (fixture->test_object));
  g_test_assert_expected_messages ();
}

static void
test_permissions_provider_creates_config_file_if_absent (Fixture      *fixture,
                                                         gconstpointer unused)
{
  g_assert (g_file_query_exists (fixture->temp_file, NULL));
}

static void
test_permissions_provider_reloads_changed_config_file (Fixture      *fixture,
                                                       gconstpointer unused)
{
  g_assert (emer_permissions_provider_get_daemon_enabled (fixture->test_object));
  g_assert (g_file_replace_contents (fixture->temp_file,
                                     CONFIG_FILE_DISABLED_CONTENTS,
                                     strlen (CONFIG_FILE_DISABLED_CONTENTS),
                                     NULL /* etag */, FALSE /* backup */,
                                     G_FILE_CREATE_NONE, NULL /* new etag */,
                                     NULL, NULL));
  g_main_loop_run (fixture->main_loop);
  g_assert (fixture->notify_daemon_called);
  g_assert_false (fixture->notify_daemon_called_with);
  g_assert_false (emer_permissions_provider_get_daemon_enabled (fixture->test_object));
}

static void
test_permissions_provider_loads_created_config_file (Fixture      *fixture,
                                                     gconstpointer unused)
{
  g_assert_false (emer_permissions_provider_get_daemon_enabled (fixture->test_object));
  g_assert (g_file_replace_contents (fixture->temp_file,
                                     CONFIG_FILE_ENABLED_CONTENTS,
                                     strlen (CONFIG_FILE_ENABLED_CONTENTS),
                                     NULL /* etag */, FALSE /* backup */,
                                     G_FILE_CREATE_NONE, NULL /* new etag */,
                                     NULL, NULL));
  g_main_loop_run (fixture->main_loop);
  g_assert (fixture->notify_daemon_called);
  g_assert (fixture->notify_daemon_called_with);
  g_assert (emer_permissions_provider_get_daemon_enabled (fixture->test_object));
}

static void
test_permissions_provider_recreates_deleted_config_file (Fixture      *fixture,
                                                         gconstpointer unused)
{
  g_assert (g_file_delete (fixture->temp_file, NULL, NULL));
  g_main_loop_run (fixture->main_loop);
  g_assert (fixture->notify_daemon_called);
  g_assert_false (fixture->notify_daemon_called_with);
  g_assert_false (emer_permissions_provider_get_daemon_enabled (fixture->test_object));

  /* We don't block on recreating the config file, so if we were to assert that
  it had been created, we couldn't guarantee that it had finished being
  recreated by the time an assertion happened. So, less than ideally, the test
  has to be satisfied with the fact that notify::daemon-enabled was called after
  the file was deleted. */
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
  g_assert (fixture->notify_daemon_called);
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
  g_assert (g_file_load_contents (fixture->temp_file, NULL, &contents, NULL,
                                  NULL, NULL));
  g_assert (strstr (contents, "enabled=true"));
  g_free (contents);

  g_idle_add ((GSourceFunc) set_daemon_enabled_false_idle, fixture);

  GFileMonitor *monitor = g_file_monitor_file (fixture->temp_file,
                                               G_FILE_MONITOR_NONE, NULL, NULL);
  g_assert_nonnull (monitor);

  g_signal_connect (monitor, "changed", G_CALLBACK (on_config_file_changed),
                    fixture);

  /* Run the main loop twice, to wait for two events: the property notification
  and a "changed" or "created" file monitor event. */
  g_main_loop_run (fixture->main_loop);
  g_main_loop_run (fixture->main_loop);

  g_object_unref (monitor);

  g_assert (g_file_load_contents (fixture->temp_file, NULL, &contents, NULL,
                                  NULL, NULL));
  g_assert (strstr (contents, "enabled=false"));
  g_free (contents);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);

#define ADD_PERMISSIONS_PROVIDER_TEST(path, file_contents, setup, test_func) \
  g_test_add ((path), Fixture, (file_contents), (setup), (test_func), teardown);

  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/new/existing-config-file",
                                 CONFIG_FILE_ENABLED_CONTENTS, setup,
                                 test_permissions_provider_new);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/new/absent-config-file",
                                 NULL, setup, test_permissions_provider_new);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/new/invalid-config-file",
                                 CONFIG_FILE_INVALID_CONTENTS,
                                 setup_invalid_file,
                                 test_permissions_provider_new_invalid_file);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-daemon-enabled/existing-config-file-yes",
                                 CONFIG_FILE_ENABLED_CONTENTS, setup,
                                 test_permissions_provider_get_daemon_enabled);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-daemon-enabled/existing-config-file-no",
                                 CONFIG_FILE_DISABLED_CONTENTS, setup,
                                 test_permissions_provider_get_daemon_enabled_false);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-daemon-enabled/absent-config-file",
                                 NULL, setup,
                                 test_permissions_provider_get_daemon_enabled_fallback);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/get-daemon-enabled/invalid-config-file",
                                 CONFIG_FILE_INVALID_CONTENTS,
                                 setup_invalid_file,
                                 test_permissions_provider_get_daemon_enabled_fallback);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/creates-config-file-if-absent",
                                 NULL, setup,
                                 test_permissions_provider_creates_config_file_if_absent);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/reloads-changed-config-file",
                                 CONFIG_FILE_ENABLED_CONTENTS, setup,
                                 test_permissions_provider_reloads_changed_config_file);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/loads-created-config-file",
                                 NULL, setup,
                                 test_permissions_provider_loads_created_config_file);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/recreates-deleted-config-file",
                                 CONFIG_FILE_ENABLED_CONTENTS, setup,
                                 test_permissions_provider_recreates_deleted_config_file);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/set-daemon-enabled",
                                 CONFIG_FILE_ENABLED_CONTENTS, setup,
                                 test_permissions_provider_set_daemon_enabled);
  ADD_PERMISSIONS_PROVIDER_TEST ("/permissions-provider/set-daemon-enabled-updates-config-file",
                                 CONFIG_FILE_ENABLED_CONTENTS, setup,
                                 test_permissions_provider_set_daemon_enabled_updates_config_file);

#undef ADD_PERMISSIONS_PROVIDER_TEST

  return g_test_run ();
}
