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
mock_web_send_assert_sync (const gchar  *uri,
                           const gchar  *data,
                           const gchar  *username,
                           const gchar  *password,
                           GCancellable *cancellable,
                           GError      **error)
{
  g_assert_cmpstr (uri, ==, EXPECTED_ENDPOINT "/foobar");
  g_assert_cmpstr (data, ==, EXPECTED_SENT_DATA);
  g_assert_cmpstr (username, ==, EXPECTED_USERNAME);
  g_assert_cmpstr (password, ==, EXPECTED_PASSWORD);
  return TRUE;
}

static void
mock_web_send_assert_async (const gchar        *uri,
                            const gchar        *data,
                            const gchar        *username,
                            const gchar        *password,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            callback_data)
{
  mock_web_send_assert_sync (uri, data, username, password, NULL, NULL);
  mock_web_send_async (uri, data, username, password,
                       NULL, callback, callback_data);
}

static gboolean
mock_web_send_assert_fingerprint_sync (const gchar  *uri,
                                       const gchar  *data,
                                       const gchar  *username,
                                       const gchar  *password,
                                       GCancellable *cancellable,
                                       GError      **error)
{
  g_assert (strstr (data, "\"fingerprint\":\"" MOCK_FINGERPRINT "\"") != NULL);
  return TRUE;
}

static void
mock_web_send_assert_fingerprint_async (const gchar        *uri,
                                        const gchar        *data,
                                        const gchar        *username,
                                        const gchar        *password,
                                        GCancellable       *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer            callback_data)
{
  mock_web_send_assert_fingerprint_sync (uri, data, username, password,
                                         NULL, NULL);
  mock_web_send_async (uri, data, username, password,
                       NULL, callback, callback_data);
}

struct ConnectionFixture {
  GFile *tmpdir;
  GFile *fingerprint_file;
  GFile *endpoint_file;
  EmtrConnection *test_object;
  GMainLoop *mainloop; /* for async tests */
};

static void
setup (struct ConnectionFixture *fixture,
       gconstpointer             unused)
{
  gchar *tmpdirpath = g_mkdtemp (g_strdup (TMP_DIRECTORY_TEMPLATE));
  fixture->tmpdir = g_file_new_for_path (tmpdirpath);
  fixture->fingerprint_file = g_file_get_child (fixture->tmpdir, "fingerprint");
  fixture->endpoint_file = g_file_get_child (fixture->tmpdir, "endpoint.json");
  g_assert (g_file_replace_contents (fixture->endpoint_file,
                                     MOCK_ENDPOINT_FILE_CONTENTS,
                                     strlen (MOCK_ENDPOINT_FILE_CONTENTS), NULL,
                                     FALSE, G_FILE_CREATE_REPLACE_DESTINATION,
                                     NULL, NULL, NULL));
  g_free (tmpdirpath);

  fixture->test_object = g_object_new (EMTR_TYPE_CONNECTION,
                                       "fingerprint-file", fixture->fingerprint_file,
                                       NULL);

  fixture->test_object->_uuid_gen_func = mock_uuid;
  fixture->test_object->_mac_gen_func = mock_mac;
  fixture->test_object->_web_send_sync_func = mock_web_send_sync;
  fixture->test_object->_web_send_async_func = mock_web_send_async;
  fixture->test_object->_web_send_finish_func = mock_web_send_finish;

  fixture->mainloop = g_main_loop_new (NULL, FALSE);
}

static void
teardown (struct ConnectionFixture *fixture,
          gconstpointer             unused)
{
  /* Remove temp dir */
  g_assert (g_file_delete (fixture->endpoint_file, NULL, NULL));
  g_object_unref (fixture->endpoint_file);
  g_file_delete (fixture->fingerprint_file, NULL, NULL); /* ignore error */
  g_object_unref (fixture->fingerprint_file);
  g_assert (g_file_delete (fixture->tmpdir, NULL, NULL));
  g_object_unref (fixture->tmpdir);

  g_object_unref (fixture->test_object);
  g_main_loop_unref (fixture->mainloop);
}

static void
test_connection_returns_true_if_data_sent_successfully (struct ConnectionFixture *fixture,
                                                        gconstpointer             unused)
{
  GError *error = NULL;
  GVariant *payload = create_payload ("foo", 12345, TRUE);
  gboolean success = emtr_connection_send_sync (fixture->test_object, payload,
                                                NULL, &error);
  g_variant_unref (payload);

  g_assert (success);
  g_assert_no_error (error);
}

static void
test_connection_returns_error_if_data_not_sent_successfully (struct ConnectionFixture *fixture,
                                                             gconstpointer             unused)
{
  GError *error = NULL;
  fixture->test_object->_web_send_sync_func = mock_web_send_exception_sync;
  GVariant *payload = create_payload ("foo", 1234, TRUE);
  gboolean success = emtr_connection_send_sync (fixture->test_object, payload,
                                                NULL, &error);
  g_variant_unref (payload);

  g_assert (!success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_error_free (error);
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
  g_object_unref (fixture->test_object);
  fixture->test_object = g_object_new (EMTR_TYPE_CONNECTION,
                                       "uri-context", "foobar",
                                       "form-param-name", "foobaz",
                                       "endpoint-config-file", fixture->endpoint_file,
                                       NULL);
  fixture->test_object->_uuid_gen_func = mock_uuid;
  fixture->test_object->_mac_gen_func = mock_mac;
  fixture->test_object->_web_send_sync_func = mock_web_send_assert_sync;

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  g_assert (emtr_connection_send_sync (fixture->test_object, payload,
                                       NULL, NULL));
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
  fixture->test_object->_web_send_sync_func = mock_web_send_assert_fingerprint_sync;
  g_assert (emtr_connection_send_sync (fixture->test_object, payload,
                                       NULL, NULL));
  g_variant_unref (payload);

  /* Other assertions in mock_web_send_assert_fingerprint() */
}

static void
test_connection_getting_fingerprint_creates_file_if_it_doesnt_exist (struct ConnectionFixture *fixture,
                                                                     gconstpointer             unused)
{
  g_assert (!g_file_query_exists (fixture->fingerprint_file, NULL));

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  g_assert (emtr_connection_send_sync (fixture->test_object, payload,
                                       NULL, NULL));
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
  g_assert (emtr_connection_send_sync (fixture->test_object, payload,
                                       NULL, NULL));
  g_variant_unref (payload);

  g_assert (mock_uuid_called);
  g_assert (mock_mac_called);
}

static void
async_returns_true_if_data_sent_successfully_done (EmtrConnection           *conn,
                                                   GAsyncResult             *result,
                                                   struct ConnectionFixture *fixture)
{
  GError *error = NULL;
  gboolean success = emtr_connection_send_finish (conn, result, &error);

  g_assert (success);
  g_assert_no_error (error);
  g_main_loop_quit (fixture->mainloop);
}

static void
test_connection_async_returns_true_if_data_sent_successfully (struct ConnectionFixture *fixture,
                                                              gconstpointer             unused)
{
  GVariant *payload = create_payload ("foo", 12345, TRUE);
  emtr_connection_send (fixture->test_object, payload, NULL,
                        (GAsyncReadyCallback)async_returns_true_if_data_sent_successfully_done,
                        fixture);
  g_main_loop_run (fixture->mainloop);
  g_variant_unref (payload);
}

static void
async_returns_error_if_data_not_sent_successfully_done (EmtrConnection           *conn,
                                                        GAsyncResult             *result,
                                                        struct ConnectionFixture *fixture)
{
  GError *error = NULL;
  gboolean success = emtr_connection_send_finish (conn, result, &error);

  g_assert (!success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_error_free (error);
  g_main_loop_quit (fixture->mainloop);
}

static void
test_connection_async_returns_error_if_data_not_sent_successfully (struct ConnectionFixture *fixture,
                                                                   gconstpointer             unused)
{
  fixture->test_object->_web_send_async_func = mock_web_send_exception_async;
  GVariant *payload = create_payload ("foo", 1234, TRUE);
  emtr_connection_send (fixture->test_object, payload, NULL,
                        (GAsyncReadyCallback)async_returns_error_if_data_not_sent_successfully_done,
                        fixture);
  g_main_loop_run (fixture->mainloop);
  g_variant_unref (payload);
}

static void
test_connection_async_generic_callback (EmtrConnection           *conn,
                                        GAsyncResult             *result,
                                        struct ConnectionFixture *fixture)
{
  g_assert (emtr_connection_send_finish (conn, result, NULL));
  g_main_loop_quit (fixture->mainloop);
}

static void
test_connection_async_makes_correct_send_call (struct ConnectionFixture *fixture,
                                               gconstpointer             unused)
{
  g_object_unref (fixture->test_object);
  fixture->test_object = g_object_new (EMTR_TYPE_CONNECTION,
                                       "uri-context", "foobar",
                                       "form-param-name", "foobaz",
                                       "endpoint-config-file", fixture->endpoint_file,
                                       NULL);
  fixture->test_object->_uuid_gen_func = mock_uuid;
  fixture->test_object->_mac_gen_func = mock_mac;
  fixture->test_object->_web_send_async_func = mock_web_send_assert_async;

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  emtr_connection_send (fixture->test_object, payload, NULL,
                        (GAsyncReadyCallback)test_connection_async_generic_callback,
                        fixture);
  g_main_loop_run (fixture->mainloop);
  g_variant_unref (payload);

  /* Other assertions in mock_web_send_assert_async() */
}

static void
test_connection_async_get_fingerprint_returns_contents_of_file (struct ConnectionFixture *fixture,
                                                                gconstpointer             unused)
{
  g_assert (g_file_replace_contents (fixture->fingerprint_file,
                                     MOCK_FINGERPRINT,
                                     strlen (MOCK_FINGERPRINT), NULL, FALSE,
                                     G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                     NULL, NULL));
  GVariant *payload = create_payload ("foo", 1234, TRUE);
  fixture->test_object->_web_send_async_func = mock_web_send_assert_fingerprint_async;
  emtr_connection_send (fixture->test_object, payload, NULL,
                        (GAsyncReadyCallback)test_connection_async_generic_callback,
                        fixture);
  g_main_loop_run (fixture->mainloop);
  g_variant_unref (payload);

  /* Other assertions in mock_web_send_assert_fingerprint_async() */
}

static void
test_connection_async_getting_fingerprint_creates_file_if_it_doesnt_exist (struct ConnectionFixture *fixture,
                                                                           gconstpointer             unused)
{
  g_assert (!g_file_query_exists (fixture->fingerprint_file, NULL));

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  emtr_connection_send (fixture->test_object, payload, NULL,
                        (GAsyncReadyCallback)test_connection_async_generic_callback,
                        fixture);
  g_main_loop_run (fixture->mainloop);
  g_variant_unref (payload);

  g_assert (g_file_query_exists (fixture->fingerprint_file, NULL));
}

static void
test_connection_async_sending_metrics_get_uuid_and_mac_address (struct ConnectionFixture *fixture,
                                                                gconstpointer             unused)
{
  mock_uuid_called = FALSE;
  mock_mac_called = FALSE;

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  emtr_connection_send (fixture->test_object, payload, NULL,
                        (GAsyncReadyCallback)test_connection_async_generic_callback,
                        fixture);
  g_main_loop_run (fixture->mainloop);
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

  ADD_CONNECTION_TEST_FUNC ("/connection/sync/returns-true-if-data-sent-successfully",
                            test_connection_returns_true_if_data_sent_successfully);
  ADD_CONNECTION_TEST_FUNC ("/connection/sync/returns-error-if-data-not-sent-successfully",
                            test_connection_returns_error_if_data_not_sent_successfully);
  ADD_CONNECTION_TEST_FUNC ("/connection/default-endpoint-is-localhost",
                            test_connection_default_endpoint_is_localhost);
  ADD_CONNECTION_TEST_FUNC ("/connection/sync/makes-correct-send-call",
                            test_connection_makes_correct_send_call);
  ADD_CONNECTION_TEST_FUNC ("/connection/sync/get-fingerprint-returns-contents-of-file",
                            test_connection_get_fingerprint_returns_contents_of_file);
  ADD_CONNECTION_TEST_FUNC ("/connection/sync/getting-fingerprint-creates-file-if-it-doesnt-exist",
                            test_connection_getting_fingerprint_creates_file_if_it_doesnt_exist);
  ADD_CONNECTION_TEST_FUNC ("/connection/sync/sending-metrics-gets-uuid-and-mac-address",
                            test_connection_sending_metrics_get_uuid_and_mac_address);
  ADD_CONNECTION_TEST_FUNC ("/connection/async/returns-true-if-data-sent-successfully",
                            test_connection_async_returns_true_if_data_sent_successfully);
  ADD_CONNECTION_TEST_FUNC ("/connection/async/returns-error-if-data-not-sent-successfully",
                            test_connection_async_returns_error_if_data_not_sent_successfully);
  ADD_CONNECTION_TEST_FUNC ("/connection/async/makes-correct-send-call",
                            test_connection_async_makes_correct_send_call);
  ADD_CONNECTION_TEST_FUNC ("/connection/async/get-fingerprint-returns-contents-of-file",
                            test_connection_async_get_fingerprint_returns_contents_of_file);
  ADD_CONNECTION_TEST_FUNC ("/connection/async/getting-fingerprint-creates-file-if-it-doesnt-exist",
                            test_connection_async_getting_fingerprint_creates_file_if_it_doesnt_exist);
  ADD_CONNECTION_TEST_FUNC ("/connection/async/sending-metrics-gets-uuid-and-mac-address",
                            test_connection_async_sending_metrics_get_uuid_and_mac_address);

#undef ADD_CONNECTION_TEST_FUNC
}
