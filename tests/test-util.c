/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <glib.h>
#include <gio/gio.h>

#include <eosmetrics/emtr-util.h>
#include <eosmetrics/emtr-util-private.h>
#include "run-tests.h"

#define MOCK_APP_ID "com.example.smokegrinder"
#define MOCK_FEEDBACK_MSG "The grinder doesn't grind smoke."

static void
test_util_data_dir_not_null (void)
{
  GFile *file = emtr_get_data_dir ();
  g_assert (file != NULL);
  g_object_unref (file);
}

static void
test_util_storage_dir_not_null (void)
{
  GFile *file = emtr_get_default_storage_dir ();
  g_assert (file != NULL);
  g_object_unref (file);
}

static void
test_util_session_time_payload_is_floating (void)
{
  set_up_mock_version_file (MOCK_VERSION_FILE_CONTENTS);
  GVariant *payload = emtr_create_session_time_payload (1);
  g_assert (g_variant_is_floating (payload));
  g_variant_unref (payload);
}

static void
test_util_session_time_payload_has_expected_keys (void)
{
  gint64 elapsed;
  const gchar *version_string;

  set_up_mock_version_file (MOCK_VERSION_FILE_CONTENTS);

  GVariant *payload = emtr_create_session_time_payload (1);
  g_assert (g_variant_is_of_type (payload, G_VARIANT_TYPE_VARDICT));
  GVariant *inner_dict = g_variant_lookup_value (payload, "session_time",
                                                 G_VARIANT_TYPE_VARDICT);
  g_assert (inner_dict != NULL);
  g_assert (g_variant_lookup (inner_dict, "time_in_operating_system",
                              "x", &elapsed));
  g_assert_cmpint (elapsed, ==, 1);
  g_assert (g_variant_lookup (inner_dict, "os_version", "&s", &version_string));
  g_assert (g_str_has_prefix (version_string, "EndlessOS "));
  g_variant_unref (payload);
}

static void
test_util_app_usage_payload_is_floating (void)
{
  GVariant *payload = emtr_create_app_usage_payload (MOCK_APP_ID, 1);
  g_assert (g_variant_is_floating (payload));
  g_variant_unref (payload);
}

static void
test_util_app_usage_payload_has_expected_keys (void)
{
  const gchar *app_id = NULL;
  gint64 elapsed = 0, timestamp = 0;

  GVariant *payload = emtr_create_app_usage_payload (MOCK_APP_ID, 1);
  g_assert (g_variant_is_of_type (payload, G_VARIANT_TYPE_VARDICT));
  g_assert (g_variant_lookup (payload, "activityName", "&s", &app_id));
  g_assert_cmpstr (app_id, ==, MOCK_APP_ID);
  g_assert (g_variant_lookup (payload, "timeSpentInActivity", "x", &elapsed));
  g_assert_cmpint (elapsed, ==, 1);
  g_assert (g_variant_lookup (payload, "timestamp", "x", &timestamp));
  /* Assert a sane value for the timestamp */
  g_assert_cmpint (timestamp, >, 0);
  g_variant_unref (payload);
}

static void
test_util_feedback_payload_is_floating (void)
{
  GVariant *payload = emtr_create_feedback_payload (MOCK_FEEDBACK_MSG, TRUE);
  g_assert (g_variant_is_floating (payload));
  g_variant_unref (payload);
}

static void
test_util_feedback_payload_has_expected_keys (void)
{
  const gchar *message = NULL;
  gboolean is_bug = FALSE;
  gint64 timestamp = 0;

  GVariant *payload = emtr_create_feedback_payload (MOCK_FEEDBACK_MSG, TRUE);
  g_assert (g_variant_is_of_type (payload, G_VARIANT_TYPE_VARDICT));
  g_assert (g_variant_lookup (payload, "message", "&s", &message));
  g_assert_cmpstr (message, ==, MOCK_FEEDBACK_MSG);
  g_assert (g_variant_lookup (payload, "bug", "b", &is_bug));
  g_assert (is_bug);
  g_assert (g_variant_lookup (payload, "timestamp", "x", &timestamp));
  /* Assert a sane value for the timestamp */
  g_assert_cmpint (timestamp, >, 0);
  g_variant_unref (payload);
}

static void
test_util_aggregate_payload_is_floating (void)
{
  GVariant *empty = NULL;
  GVariant *payload = emtr_aggregate_app_usage_payloads (&empty);
  g_assert (g_variant_is_floating (payload));
  g_variant_unref (payload);
}

static void
test_util_aggregate_payload_contains_original_payloads (void)
{
  gchar *buffer;
  GVariant *payloads[3] = {
    g_variant_new_string ("0"),
    g_variant_new_string ("1"),
    NULL
  };
  GVariant *temp;
  GVariant *aggregate = emtr_aggregate_app_usage_payloads (payloads);
  g_assert (g_variant_is_of_type (aggregate, G_VARIANT_TYPE_VARDICT));
  GVariant *array = g_variant_lookup_value (aggregate, "time_in_activities",
                                            G_VARIANT_TYPE_ARRAY);
  g_assert (array != NULL);
  g_assert_cmpint (g_variant_n_children (array), ==, 2);

  g_variant_get_child (array, 0, "v", &temp);
  g_variant_get (temp, "&s", &buffer);
  g_assert_cmpstr (buffer, ==, "0");
  g_variant_unref (temp);
  g_variant_get_child (array, 1, "v", &temp);
  g_variant_get (temp, "&s", &buffer);
  g_assert_cmpstr (buffer, ==, "1");
  g_variant_unref (temp);

  g_variant_unref (aggregate);
}

void
add_util_tests (void)
{
  g_test_add_func ("/util/data-dir-not-null",
                   test_util_data_dir_not_null);
  g_test_add_func ("/util/storage-dir-not-null",
                   test_util_storage_dir_not_null);
  g_test_add_func ("/util/session-time-payload-is-floating",
                   test_util_session_time_payload_is_floating);
  g_test_add_func ("/util/session-time-payload-has-expected-keys",
                   test_util_session_time_payload_has_expected_keys);
  g_test_add_func ("/util/app-usage-payload-is-floating",
                   test_util_app_usage_payload_is_floating);
  g_test_add_func ("/util/app-usage-payload-has-expected-keys",
                   test_util_app_usage_payload_has_expected_keys);
  g_test_add_func ("/util/feedback-payload-is-floating",
                   test_util_feedback_payload_is_floating);
  g_test_add_func ("/util/feedback-payload-has-expected-keys",
                   test_util_feedback_payload_has_expected_keys);
  g_test_add_func ("/util/aggregate/payload-is-floating",
                   test_util_aggregate_payload_is_floating);
  g_test_add_func ("/util/aggregate/payload-contains-original-payloads",
                   test_util_aggregate_payload_contains_original_payloads);
}
