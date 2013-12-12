/* Copyright 2013 Endless Mobile, Inc. */

#include <glib.h>
#include <eosmetrics/eosmetrics.h>

#include "run-tests.h"

gboolean
mock_web_send (const gchar *uri,
               const gchar *data,
               const gchar *username,
               const gchar *password,
               GError     **error)
{
  return TRUE;
}

gboolean
mock_web_send_exception (const gchar *uri,
                         const gchar *data,
                         const gchar *username,
                         const gchar *password,
                         GError     **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Mock message");
  return FALSE;
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

  return g_test_run ();
}
