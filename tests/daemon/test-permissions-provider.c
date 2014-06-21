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
} Fixture;

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
  g_file_delete (fixture->temp_file, NULL, NULL); /* might not exist */
  g_clear_object (&fixture->temp_file);
  g_clear_object (&fixture->test_object);
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

#undef ADD_PERMISSIONS_PROVIDER_TEST

  return g_test_run ();
}
