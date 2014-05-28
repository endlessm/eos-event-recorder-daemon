/* Copyright 2013 Endless Mobile, Inc. */

#include <glib.h>
#include <eosmetrics/eosmetrics.h>

#include "run-tests.h"

gboolean
mock_web_send_sync (const gchar  *uri,
                    const gchar  *data,
                    const gchar  *username,
                    const gchar  *password,
                    GCancellable *cancellable,
                    GError      **error)
{
  return TRUE;
}

void
mock_web_send_async (const gchar        *uri,
                     const gchar        *data,
                     const gchar        *username,
                     const gchar        *password,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            callback_data)
{
  GTask *task = g_task_new (NULL, NULL, callback, callback_data);
  g_task_return_boolean (task, TRUE);
}

gboolean
mock_web_send_finish (GAsyncResult *result,
                      GError      **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
mock_web_send_exception_sync (const gchar  *uri,
                              const gchar  *data,
                              const gchar  *username,
                              const gchar  *password,
                              GCancellable *cancellable,
                              GError      **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Mock message");
  return FALSE;
}

void
mock_web_send_exception_async (const gchar        *uri,
                               const gchar        *data,
                               const gchar        *username,
                               const gchar        *password,
                               GCancellable       *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer            callback_data)
{
  GTask *task = g_task_new (NULL, NULL, callback, callback_data);
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Mock message");
}

/* Returns a GVariant with a floating reference */
GVariant *
create_payload (const gchar *message,
                gint64       timestamp,
                gboolean     is_bug)
{
  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}",
                         "message", g_variant_new_string (message));
  g_variant_builder_add (&builder, "{sv}",
                         "timestamp", g_variant_new_int64 (timestamp));
  g_variant_builder_add (&builder, "{sv}",
                         "bug", g_variant_new_boolean (is_bug));
  return g_variant_builder_end (&builder);
}

void
set_up_mock_version_file (const gchar *contents)
{
  const gchar *version_filename = g_getenv (MOCK_VERSION_FILE_ENVVAR);
  g_assert (g_file_set_contents (version_filename, contents, -1, NULL));
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  add_util_tests ();
  add_osversion_tests ();
  add_uuid_tests ();
  add_mac_tests ();
  add_web_tests ();
  add_connection_tests ();
  add_sender_tests ();
  add_event_recorder_tests ();
  add_persistent_cache_tests ();

  return g_test_run ();
}
