/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-util.h"
#include "emtr-util-private.h"
#include "emtr-osversion-private.h"

#include <glib.h>
#include <gio/gio.h>

/**
 * SECTION:emtr-util
 * @title: General
 * @short_description: General metrics functions
 * @include: eosmetrics/eosmetrics.h
 *
 * These are general functions available in the metrics kit.
 *
 * Use emtr_get_default_storage_dir() to get a handle to the directory where
 * metrics data is queued up for sending, in case you want to examine the queue
 * yourself.
 */

/* Returns a GVariant with floating reference, type int64, with the current time
in seconds */
static GVariant *
get_current_time_variant (void)
{
  return g_variant_new_int64 (g_get_real_time () / G_USEC_PER_SEC);
}

/* Internal "public" API */

/*
 * emtr_get_data_dir:
 *
 * Library-private function.
 *
 * Returns: (transfer full): a #GFile pointing to $XDG_DATA_HOME/eosmetrics
 */
GFile *
emtr_get_data_dir (void)
{
  const gchar *user = g_get_user_data_dir ();
  GFile *user_dir = g_file_new_for_path (user);
  GFile *retval = g_file_get_child (user_dir, "eosmetrics");
  g_object_unref (user_dir);
  return retval;
}

/* Public API */

/**
 * emtr_get_default_storage_dir:
 *
 * Retrieves the default directory where metrics data is queued up for sending
 * if it couldn't be sent immediately.
 * Usually you won't need to use this function unless you are doing something
 * tricky like inserting items into the queue yourself.
 *
 * Returns: (transfer full): a #GFile pointing to the directory.
 * Free with g_object_unref() when done.
 */
GFile *
emtr_get_default_storage_dir (void)
{
  GFile *data_dir = emtr_get_data_dir ();
  GFile *retval = g_file_get_child (data_dir, "storage");
  g_object_unref (data_dir);
  return retval;
}

/**
 * emtr_create_session_time_payload:
 * @elapsed_time: the time spent in the session, in seconds
 *
 * Convenience function to create a #GVariant dictionary that sends the session
 * time.
 * Use with emtr_sender_new_for_session_metrics().
 * The format looks like this:
 * |[
 * {
 *     "session_time": {
 *         "time_in_operating_system": (int64),
 *         "os_version": (string)
 *     }
 * }
 * ]|
 *
 * Returns: (transfer full): a #GVariant with a floating reference.
 * Free with g_variant_unref() when done.
 */
GVariant *
emtr_create_session_time_payload (gint64 elapsed_time)
{
  gchar *os_version = emtr_get_os_version ();
  gchar *formatted_os_version = g_strconcat ("EndlessOS ", os_version, NULL);
  g_free (os_version);

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}",
                         "time_in_operating_system", g_variant_new_int64 (elapsed_time));
  g_variant_builder_add (&builder, "{sv}",
                         "os_version", g_variant_new_string (formatted_os_version));
  GVariant *inner_dict = g_variant_builder_end (&builder);  /* floating ref */

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}",
                         "session_time", inner_dict);  /* sinks ref */
  return g_variant_builder_end (&builder);
}

/**
 * emtr_create_app_usage_payload:
 * @activity_name: an identifying string for the application, suggested to be
 * the application ID (see #GApplication:application-id)
 * @elapsed_time: the time spent in the application, in seconds
 *
 * Convenience function to create a #GVariant dictionary containing application
 * usage data.
 * Use with emtr_sender_new_for_app_usage_metrics().
 *
 * The format looks like this:
 * |[
 * {
 *     "activityName": (string),
 *     "timeSpentInActivity": (int64),
 *     "timestamp": (int64)
 * }
 * ]|
 *
 * Returns: (transfer full): a #GVariant with a floating reference.
 * Free with g_variant_unref() when done.
 */
GVariant *
emtr_create_app_usage_payload (const gchar *activity_name,
                               gint64       elapsed_time)
{
  g_return_val_if_fail (activity_name != NULL, NULL);

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}",
                         "activityName", g_variant_new_string (activity_name));
  g_variant_builder_add (&builder, "{sv}",
                         "timeSpentInActivity",
                         g_variant_new_int64 (elapsed_time));
  g_variant_builder_add (&builder, "{sv}",
                         "timestamp", get_current_time_variant ());
  return g_variant_builder_end (&builder);
}

/**
 * emtr_aggregate_app_usage_payloads:
 * @payloads: (array zero-terminated=1) (element-type GVariant) (transfer none):
 * a %NULL-terminated array of #GVariant<!---->s to aggregate
 *
 * Aggregates several payloads created by emtr_create_app_usage_payload() into
 * one.
 * Use with emtr_sender_new_for_app_usage_metrics().
 *
 * The format looks like this:
 * |[
 * {
 *     "time_in_activities": [
 *         (payload),
 *         (payload),
 *         ...
 *     ]
 * }
 * ]|
 *
 * Returns: (transfer full): a #GVariant with a floating reference.
 * Free with g_variant_unref() when done.
 */
GVariant *
emtr_aggregate_app_usage_payloads (GVariant **payloads)
{
  g_return_val_if_fail (payloads != NULL, NULL);

  GVariantBuilder builder;
  g_variant_builder_init (&builder, (const GVariantType *)"av");

  GVariant **payload;
  for (payload = payloads; *payload != NULL; payload++)
    g_variant_builder_add (&builder, "v", *payload); /* takes ref */

  GVariant *inner_dict = g_variant_builder_end (&builder);  /* floating ref */

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}",
                         "time_in_activities", inner_dict);  /* sinks ref */
  return g_variant_builder_end (&builder);
}

/**
 * emtr_create_feedback_payload:
 * @message: a string containing user feedback
 * @is_bug: %TRUE if the feedback is a bug report
 *
 * Convenience function to create a #GVariant dictionary containing user
 * feedback.
 * Use with emtr_sender_new_for_feedback().
 *
 * The format looks like this:
 * |[
 * {
 *     "message": (string),
 *     "timestamp": (int64),
 *     "bug": (boolean)
 * }
 * ]|
 *
 * Returns: (transfer full): a #GVariant with a floating reference.
 * Free with g_variant_unref() when done.
 */
GVariant *
emtr_create_feedback_payload (const gchar *message,
                              gboolean     is_bug)
{
  g_return_val_if_fail (message != NULL, NULL);

  GVariantBuilder builder;
  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}",
                         "message", g_variant_new_string (message));
  g_variant_builder_add (&builder, "{sv}",
                         "timestamp", get_current_time_variant ());
  g_variant_builder_add (&builder, "{sv}",
                         "bug", g_variant_new_boolean (is_bug));
  return g_variant_builder_end (&builder);
}
