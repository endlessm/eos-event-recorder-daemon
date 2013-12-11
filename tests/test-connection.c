/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <string.h>
#include <glib.h>

#include <eosmetrics/emtr-connection.h>
#include "run-tests.h"

#define TMP_DIRECTORY_TEMPLATE "/tmp/metricsconnectiontestXXXXXX"
#define MOCK_UUID_VALUE "123"
#define MOCK_MAC_VALUE ((gint64)321)
#define MOCK_JSON_PAYLOAD "{\"message\":\"foo\",\"timestamp\":12345,\"bug\":true}"
#define MOCK_USERNAME "user"
#define MOCK_PASSWORD "pass"
#define EXPECTED_ENDPOINT "http://testendpoint:9999"
#define MOCK_ENDPOINT_FILE_CONTENTS "{\"endpoint\":\"" EXPECTED_ENDPOINT "\"}"
#define EXPECTED_SENT_DATA "{\"foobaz\":{\"message\":\"foo\",\"timestamp\":1234,\"bug\":true,\"fingerprint\":\"123\",\"machine\":321}}"
#define EXPECTED_USERNAME "endlessos"
#define EXPECTED_PASSWORD "sosseldne"
#define MOCK_FINGERPRINT "foo"

static gboolean mock_uuid_called = FALSE;
static gboolean mock_mac_called = FALSE;

static gchar *
mock_uuid (void)
{
  mock_uuid_called = TRUE;
  return g_strdup (MOCK_UUID_VALUE);
}

static gint64
mock_mac (void)
{
  mock_mac_called = TRUE;
  return MOCK_MAC_VALUE;
}

static gboolean
mock_web_send (const gchar *uri,
               const gchar *data,
               const gchar *username,
               const gchar *password,
               GError     **error)
{
  return TRUE;
}

static gboolean
mock_web_send_exception (const gchar *uri,
                         const gchar *data,
                         const gchar *username,
                         const gchar *password,
                         GError     **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Mock message");
  return FALSE;
}

static gboolean
mock_web_send_assert (const gchar *uri,
                      const gchar *data,
                      const gchar *username,
                      const gchar *password,
                      GError     **error)
{
  g_assert_cmpstr (uri, ==, EXPECTED_ENDPOINT "/foobar");
  g_assert_cmpstr (data, ==, EXPECTED_SENT_DATA);
  g_assert_cmpstr (username, ==, EXPECTED_USERNAME);
  g_assert_cmpstr (password, ==, EXPECTED_PASSWORD);
  return TRUE;
}

static gboolean
mock_web_send_assert_fingerprint (const gchar *uri,
                                  const gchar *data,
                                  const gchar *username,
                                  const gchar *password,
                                  GError     **error)
{
  g_assert (strstr (data, "\"fingerprint\":\"" MOCK_FINGERPRINT "\"") != NULL);
  return TRUE;
}

struct ConnectionFixture {
  GFile *tmpdir;
  GFile *fingerprint_file;
  EmtrConnection *test_object;
};

static void
setup (struct ConnectionFixture *fixture,
       gconstpointer             unused)
{
  gchar *tmpdirpath = g_mkdtemp (g_strdup (TMP_DIRECTORY_TEMPLATE));
  fixture->tmpdir = g_file_new_for_path (tmpdirpath);
  fixture->fingerprint_file = g_file_get_child (fixture->tmpdir, "fingerprint");
  g_free (tmpdirpath);

  fixture->test_object = g_object_new (EMTR_TYPE_CONNECTION,
                                       "fingerprint-file", fixture->fingerprint_file,
                                       NULL);

  fixture->test_object->_uuid_gen_func = mock_uuid;
  fixture->test_object->_mac_gen_func = mock_mac;
  fixture->test_object->_web_send_func = mock_web_send;
}

static void
teardown (struct ConnectionFixture *fixture,
          gconstpointer             unused)
{
  g_object_unref (fixture->tmpdir);
  g_object_unref (fixture->fingerprint_file);
  g_object_unref (fixture->test_object);
}

/* Returns a GVariant with a floating reference */
static GVariant *
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

static void
test_connection_returns_true_if_data_sent_successfully (struct ConnectionFixture *fixture,
                                                        gconstpointer             unused)
{
  GError *error = NULL;
  GVariant *payload = create_payload ("foo", 12345, TRUE);
  gboolean success = emtr_connection_send (fixture->test_object, payload,
                                           &error);
  g_variant_unref (payload);

  g_assert (success);
  g_assert_no_error (error);
}

static void
test_connection_returns_error_if_data_not_sent_successfully (struct ConnectionFixture *fixture,
                                                             gconstpointer             unused)
{
  GError *error = NULL;
  fixture->test_object->_web_send_func = mock_web_send_exception;
  GVariant *payload = create_payload ("foo", 1234, TRUE);
  gboolean success = emtr_connection_send (fixture->test_object, payload,
                                           &error);
  g_variant_unref (payload);

  g_assert (!success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
}

static void
test_connection_default_endpoint_is_localhost (struct ConnectionFixture *fixture,
                                               gconstpointer             unused)
{
  GFile *invalid_file = g_file_new_for_path("invalid_file");
  g_object_unref (fixture->test_object);
  fixture->test_object = g_object_new (EMTR_TYPE_CONNECTION,
                                       "endpoint-config-file", invalid_file,
                                       NULL);
  g_object_unref (invalid_file);

  g_assert_cmpstr (emtr_connection_get_endpoint (fixture->test_object), ==,
                   "http://localhost:3000");
}

static void
test_connection_makes_correct_send_call (struct ConnectionFixture *fixture,
                                         gconstpointer             unused)
{
  GFile *endpoint_file = g_file_get_child (fixture->tmpdir, "endpoint.json");
  g_assert (g_file_replace_contents (endpoint_file, MOCK_ENDPOINT_FILE_CONTENTS,
                                     strlen (MOCK_ENDPOINT_FILE_CONTENTS), NULL,
                                     FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                     NULL, NULL, NULL));

  g_object_unref (fixture->test_object);
  fixture->test_object = g_object_new (EMTR_TYPE_CONNECTION,
                                       "uri-context", "foobar",
                                       "form-param-name", "foobaz",
                                       "endpoint-config-file", endpoint_file,
                                       NULL);
  fixture->test_object->_uuid_gen_func = mock_uuid;
  fixture->test_object->_mac_gen_func = mock_mac;
  fixture->test_object->_web_send_func = mock_web_send_assert;

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  g_assert (emtr_connection_send (fixture->test_object, payload, NULL));
  g_variant_unref (payload);

  /* Other assertions in mock_web_send_assert() */
}

static void
test_connection_get_fingerprint_returns_contents_of_file (struct ConnectionFixture *fixture,
                                                          gconstpointer             unused)
{
  g_assert (g_file_replace_contents (fixture->fingerprint_file,
                                     MOCK_FINGERPRINT,
                                     strlen (MOCK_FINGERPRINT), NULL, FALSE,
                                     G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                     NULL, NULL));
  GVariant *payload = create_payload ("foo", 1234, TRUE);
  fixture->test_object->_web_send_func = mock_web_send_assert_fingerprint;
  g_assert (emtr_connection_send (fixture->test_object, payload, NULL));
  g_variant_unref (payload);

  /* Other assertions in mock_web_send_assert_fingerprint() */
}

static void
test_connection_getting_fingerprint_creates_file_if_it_doesnt_exist (struct ConnectionFixture *fixture,
                                                                     gconstpointer             unused)
{
  g_assert (!g_file_query_exists (fixture->fingerprint_file, NULL));

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  g_assert (emtr_connection_send (fixture->test_object, payload, NULL));
  g_variant_unref (payload);

  g_assert (g_file_query_exists (fixture->fingerprint_file, NULL));
}

static void
test_connection_sending_metrics_get_uuid_and_mac_address (struct ConnectionFixture *fixture,
                                                          gconstpointer             unused)
{
  mock_uuid_called = FALSE;
  mock_mac_called = FALSE;

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  g_assert (emtr_connection_send (fixture->test_object, payload, NULL));
  g_variant_unref (payload);

  g_assert (mock_uuid_called);
  g_assert (mock_mac_called);
}

void
add_connection_tests (void)
{
#define ADD_CONNECTION_TEST_FUNC(path, func) \
  g_test_add ((path), struct ConnectionFixture, NULL, \
              setup, (func), teardown)

  ADD_CONNECTION_TEST_FUNC ("/connection/returns-true-if-data-sent-successfully",
                            test_connection_returns_true_if_data_sent_successfully);
  ADD_CONNECTION_TEST_FUNC ("/connection/returns-error-if-data-not-sent-successfully",
                            test_connection_returns_error_if_data_not_sent_successfully);
  ADD_CONNECTION_TEST_FUNC ("/connection/default-endpoint-is-localhost",
                            test_connection_default_endpoint_is_localhost);
  ADD_CONNECTION_TEST_FUNC ("/connection/makes-correct-send-call",
                            test_connection_makes_correct_send_call);
  ADD_CONNECTION_TEST_FUNC ("/connection/get-fingerprint-returns-contents-of-file",
                            test_connection_get_fingerprint_returns_contents_of_file);
  ADD_CONNECTION_TEST_FUNC ("/connection/getting-fingerprint-creates-file-if-it-doesnt-exist",
                            test_connection_getting_fingerprint_creates_file_if_it_doesnt_exist);
  ADD_CONNECTION_TEST_FUNC ("/connection/sending-metrics-gets-uuid-and-mac-address",
                            test_connection_sending_metrics_get_uuid_and_mac_address);

#undef ADD_CONNECTION_TEST_FUNC
}
