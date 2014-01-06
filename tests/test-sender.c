/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <string.h>
#include <glib.h>

#include <eosmetrics/emtr-sender.h>
#include <eosmetrics/emtr-connection.h>
#include <eosmetrics/emtr-util.h>
#include "run-tests.h"

#define TMP_DIRECTORY_TEMPLATE "/tmp/metricssendprocesstestXXXXXX"
#define TMP_FILE_TEMPLATE "metricssendprocesstestXXXXXX.json"
#define EXPECTED_RELATIVE_FILENAME "metricssendprocesstest.json"
#define EXPECTED_DATA_QUEUE "[{\"message\":\"bar\",\"timestamp\":2002,\"bug\":false},{\"message\":\"biz\",\"timestamp\":2003,\"bug\":true}]"
#define MOCK_QUEUE "[{\"test1\":\"foo\"},{\"test2\":\"bar\"}]"
#define EXPECTED_CREATED_QUEUE "[{\"message\":\"foo\",\"timestamp\":2001,\"bug\":true}]"

static gboolean mock_web_send_assert_data_called = FALSE;

static gboolean
mock_web_send_sometimes_fail_sync (const gchar  *uri,
                                   const gchar  *data,
                                   const gchar  *username,
                                   const gchar  *password,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  static unsigned times_called = 0;
  ++times_called;
  if (times_called % 4 == 0 || times_called % 4 == 1)
    return mock_web_send_sync (uri, data, username, password, NULL, error);
  return mock_web_send_exception_sync (uri, data, username, password,
                                       NULL, error);
}

static void
mock_web_send_sometimes_fail_async (const gchar        *uri,
                                    const gchar        *data,
                                    const gchar        *username,
                                    const gchar        *password,
                                    GCancellable       *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer            callback_data)
{
  static unsigned times_called = 0;
  ++times_called;
  if (times_called % 4 == 0 || times_called % 4 == 1)
    mock_web_send_async (uri, data, username, password,
                         cancellable, callback, callback_data);
  else
    mock_web_send_exception_async (uri, data, username, password,
                                   cancellable, callback, callback_data);
}

static gboolean
mock_web_send_assert_data_sync (const gchar  *uri,
                                const gchar  *data,
                                const gchar  *username,
                                const gchar  *password,
                                GCancellable *cancellable,
                                GError      **error)
{
  mock_web_send_assert_data_called = TRUE;
  g_assert (strstr (data, "\"message\":\"foo bar\""));
  g_assert (strstr (data, "\"timestamp\":1001"));
  g_assert (strstr (data, "\"bug\":false"));
  return TRUE;
}

static void
mock_web_send_assert_data_async (const gchar        *uri,
                                 const gchar        *data,
                                 const gchar        *username,
                                 const gchar        *password,
                                 GCancellable       *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer            callback_data)
{
  mock_web_send_assert_data_sync (uri, data, username, password, NULL, NULL);
  mock_web_send_async (uri, data, username, password,
                       NULL, callback, callback_data);
}

static void
ensure_mock_queue (GFile *queue_file)
{
  g_assert (g_file_replace_contents (queue_file, MOCK_QUEUE,
                                     strlen (MOCK_QUEUE), NULL, FALSE,
                                     G_FILE_CREATE_REPLACE_DESTINATION, NULL,
                                     NULL, NULL));
}

static void
ensure_queue_dir_doesnt_exist (GFile *queue_dir)
{
  GError *error = NULL;
  gboolean success = g_file_delete (queue_dir, NULL, &error);
  g_assert (success
            || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND));
  g_clear_error (&error);
}

static gboolean
mock_web_send_assert_feedback_sync (const gchar  *uri,
                                    const gchar  *data,
                                    const gchar  *username,
                                    const gchar  *password,
                                    GCancellable *cancellable,
                                    GError      **error)
{
  mock_web_send_assert_data_called = TRUE;
  g_assert (g_str_has_prefix (data, "{\"feedback\":{"));
  g_assert (g_str_has_suffix (uri, "/feedbacks"));
  return TRUE;
}

static void
mock_web_send_assert_feedback_async (const gchar        *uri,
                                     const gchar        *data,
                                     const gchar        *username,
                                     const gchar        *password,
                                     GCancellable       *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer            callback_data)
{
  mock_web_send_assert_feedback_sync (uri, data, username, password,
                                      NULL, NULL);
  mock_web_send_async (uri, data, username, password,
                       NULL, callback, callback_data);
}

struct SenderFixture {
  GFile *tmpdir;
  GFile *storage_file;
  GFile *fingerprint_file;

  EmtrConnection *connection;
  EmtrSender *test_object;

  GMainLoop *mainloop; /* For async tests */
};

static void
setup (struct SenderFixture *fixture,
       gconstpointer         unused)
{
  mock_web_send_assert_data_called = FALSE;
  gchar *tmpdirpath = g_mkdtemp (g_strdup (TMP_DIRECTORY_TEMPLATE));
  fixture->tmpdir = g_file_new_for_path (tmpdirpath);
  fixture->storage_file = g_file_get_child (fixture->tmpdir, "data.json");
  fixture->fingerprint_file = g_file_get_child (fixture->tmpdir, "fingerprint");
  g_free (tmpdirpath);

  fixture->connection = g_object_new (EMTR_TYPE_CONNECTION,
                                      "fingerprint-file", fixture->fingerprint_file,
                                      NULL);
  fixture->connection->_web_send_sync_func = mock_web_send_sync;
  fixture->connection->_web_send_async_func = mock_web_send_async;
  fixture->connection->_web_send_finish_func = mock_web_send_finish;

  fixture->test_object = g_object_new (EMTR_TYPE_SENDER,
                                       "storage-file", fixture->storage_file,
                                       "connection", fixture->connection,
                                       NULL);

  fixture->mainloop = g_main_loop_new (NULL, FALSE);
}

static void
teardown (struct SenderFixture *fixture,
          gconstpointer         unused)
{
  g_main_loop_unref (fixture->mainloop);
  g_object_unref (fixture->tmpdir);
  g_object_unref (fixture->storage_file);
  g_object_unref (fixture->fingerprint_file);
  g_object_unref (fixture->connection);
  g_object_unref (fixture->test_object);
}

static gchar *
get_payload_from_file (EmtrSender *test_object)
{
  GFile *file = emtr_sender_get_storage_file (test_object);
  gchar *retval;
  g_assert (g_file_load_contents (file, NULL, &retval, NULL, NULL, NULL));
  return retval;
}

static void
test_sender_absolute_storage_path_is_unchanged (void)
{
  GFileIOStream *stream;
  GFile *tempfile = g_file_new_tmp (TMP_FILE_TEMPLATE, &stream, NULL);
  gchar *expected_path = g_file_get_path (tempfile);
  g_assert (tempfile != NULL);
  g_assert (g_io_stream_close (G_IO_STREAM (stream), NULL, NULL));

  EmtrSender *test_object = emtr_sender_new (tempfile);
  g_object_unref (tempfile);

  GFile *storage_file = emtr_sender_get_storage_file (test_object);
  gchar *path = g_file_get_path (storage_file);
  g_assert_cmpstr (path, ==, expected_path);
  g_assert (g_path_is_absolute (path));

  g_free (expected_path);
  g_free (path);
}

static void
test_sender_relative_storage_path_is_interpreted (void)
{
  GFile *relative = g_file_new_for_path (EXPECTED_RELATIVE_FILENAME);
  EmtrSender *test_object = emtr_sender_new (relative);
  g_object_unref (relative);

  GFile *storage_file = emtr_sender_get_storage_file (test_object);
  gchar *path = g_file_get_path (storage_file);
  g_assert_cmpstr (path, !=, EXPECTED_RELATIVE_FILENAME);
  g_assert (g_str_has_suffix (path, EXPECTED_RELATIVE_FILENAME));
  g_assert (g_path_is_absolute (path));

  g_free (path);
}

static void
test_sender_new_session_metrics_succeeds (void)
{
  EmtrSender *sender = emtr_sender_new_for_session_metrics ();
  g_assert (sender != NULL);
  g_assert (EMTR_IS_SENDER (sender));
}

static void
test_sender_new_app_metrics_succeeds (void)
{
  EmtrSender *sender = emtr_sender_new_for_app_usage_metrics ();
  g_assert (sender != NULL);
  g_assert (EMTR_IS_SENDER (sender));}

static void
test_sender_new_feedback_succeeds (void)
{
  EmtrSender *sender = emtr_sender_new_for_feedback ();
  g_assert (sender != NULL);
  g_assert (EMTR_IS_SENDER (sender));
}

static void
test_sender_invoking_send_data (struct SenderFixture *fixture,
                                gconstpointer         unused)
{
  GError *error = NULL;
  GVariant *payload = create_payload ("foo bar", 1001, FALSE);

  fixture->connection->_web_send_sync_func = mock_web_send_assert_data_sync;
  fixture->connection->_web_send_async_func = mock_web_send_assert_data_async;

  g_assert (emtr_sender_send_data_sync (fixture->test_object, payload,
                                        NULL, &error));
  g_assert_no_error (error);
  g_assert (mock_web_send_assert_data_called);

  g_variant_unref (payload);

  /* More assertions in mock_web_send_assert_data() */
}

static void
test_sender_on_failure_save_payload_to_file (struct SenderFixture *fixture,
                                             gconstpointer         unused)
{
  GError *error = NULL;
  GVariant *first = create_payload ("foo", 2001, TRUE);
  GVariant *second = create_payload ("bar", 2002, FALSE);
  GVariant *third = create_payload ("biz", 2003, TRUE);
  GVariant *fourth = create_payload ("baz", 2004, FALSE);

  fixture->connection->_web_send_sync_func = mock_web_send_sometimes_fail_sync;
  fixture->connection->_web_send_async_func = mock_web_send_sometimes_fail_async;

  g_assert (emtr_sender_send_data_sync (fixture->test_object, first,
                                        NULL, &error));
  g_assert_no_error (error);
  g_variant_unref (first);
  g_assert (emtr_sender_send_data_sync (fixture->test_object, second,
                                        NULL, &error));
  g_assert_no_error (error);
  g_variant_unref (second);
  g_assert (emtr_sender_send_data_sync (fixture->test_object, third,
                                        NULL, &error));
  g_assert_no_error (error);
  g_variant_unref (third);
  g_assert (emtr_sender_send_data_sync (fixture->test_object, fourth,
                                        NULL, &error));
  g_assert_no_error (error);
  g_variant_unref (fourth);

  gchar *loaded_payload = get_payload_from_file (fixture->test_object);
  g_assert_cmpstr (loaded_payload, ==, EXPECTED_DATA_QUEUE);
  g_free (loaded_payload);
}

static void
test_sender_cancel_send (struct SenderFixture *fixture,
                         gconstpointer         unused)
{
  GError *error = NULL;
  GVariant *payload = create_payload ("foo", 1234, TRUE);
  GCancellable *cancellable = g_cancellable_new ();

  fixture->connection->_web_send_sync_func = mock_web_send_exception_sync;
  fixture->connection->_web_send_async_func = mock_web_send_exception_async;

  g_cancellable_cancel (cancellable);
  gboolean success = emtr_sender_send_data_sync (fixture->test_object, payload,
                                                 cancellable, &error);
  g_assert (!success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_error_free (error);

  g_variant_unref (payload);
  g_object_unref (cancellable);
}

static void
test_sender_sync_sends_all_data_in_queue (struct SenderFixture *fixture,
                                          gconstpointer         unused)
{
  ensure_mock_queue (fixture->storage_file);

  GError *error = NULL;
  gboolean success = emtr_sender_send_queued_data_sync (fixture->test_object,
                                                        NULL, &error);

  g_assert (success);
  g_assert_no_error (error);
  gchar *loaded_queue = get_payload_from_file (fixture->test_object);
  g_assert_cmpstr (loaded_queue, ==, "[]");
  g_free (loaded_queue);
}

static void
test_sender_sync_requeues_data_that_still_cant_be_sent (struct SenderFixture *fixture,
                                                        gconstpointer         unused)
{
  ensure_mock_queue (fixture->storage_file);
  fixture->connection->_web_send_sync_func = mock_web_send_exception_sync;
  fixture->connection->_web_send_async_func = mock_web_send_exception_async;

  GError *error = NULL;
  gboolean success = emtr_sender_send_queued_data_sync (fixture->test_object,
                                                        NULL, &error);

  g_assert (success);
  g_assert_no_error (error);
  gchar *loaded_queue = get_payload_from_file (fixture->test_object);
  /* Again, a bit iffy because the order of the queue is not guaranteed */
  g_assert_cmpstr (loaded_queue, ==, MOCK_QUEUE);
  g_free (loaded_queue);
}

static void
test_sender_sync_send_data_deals_with_nonexistent_queue_dir (struct SenderFixture *fixture,
                                                             gconstpointer         unused)
{
  ensure_queue_dir_doesnt_exist (fixture->tmpdir);

  GError *error = NULL;
  GVariant *payload = create_payload ("foo", 2001, TRUE);

  fixture->connection->_web_send_sync_func = mock_web_send_exception_sync;
  fixture->connection->_web_send_async_func = mock_web_send_exception_async;

  g_assert (emtr_sender_send_data_sync (fixture->test_object, payload,
                                        NULL, &error));
  g_assert_no_error (error);
  g_variant_unref (payload);

  gchar *loaded_queue = get_payload_from_file (fixture->test_object);
  g_assert_cmpstr (loaded_queue, ==, EXPECTED_CREATED_QUEUE);
  g_free (loaded_queue);
}

static void
test_sender_sync_send_queued_data_deals_with_nonexistent_queue_dir (struct SenderFixture *fixture,
                                                                    gconstpointer         unused)
{
  ensure_queue_dir_doesnt_exist (fixture->tmpdir);

  GError *error = NULL;
  gboolean success = emtr_sender_send_queued_data_sync (fixture->test_object,
                                                        NULL, &error);

  g_assert (success);
  g_assert_no_error (error);
}

static void
async_invoking_send_data_done (EmtrSender           *sender,
                               GAsyncResult         *result,
                               struct SenderFixture *fixture)
{
  GError *error = NULL;
  gboolean success = emtr_sender_send_data_finish (sender, result, &error);
  g_assert (success);
  g_assert_no_error (error);
  g_assert (mock_web_send_assert_data_called);
  g_main_loop_quit (fixture->mainloop);
}

static void
test_sender_async_invoking_send_data (struct SenderFixture *fixture,
                                      gconstpointer         unused)
{
  fixture->connection->_web_send_sync_func = mock_web_send_assert_data_sync;
  fixture->connection->_web_send_async_func = mock_web_send_assert_data_async;

  GVariant *payload = create_payload ("foo bar", 1001, FALSE);
  emtr_sender_send_data (fixture->test_object, payload, NULL,
                         (GAsyncReadyCallback)async_invoking_send_data_done,
                         fixture);
  g_main_loop_run (fixture->mainloop);
  g_variant_unref (payload);

  /* More assertions in mock_web_send_assert_data() */
}

static void
async_on_failure_save_payload_to_file_done (EmtrSender           *sender,
                                            GAsyncResult         *result,
                                            struct SenderFixture *fixture)
{
  static int call_count = 0;
  GError *error = NULL;
  gboolean success = emtr_sender_send_data_finish (sender, result, &error);
  ++call_count;
  g_assert (success);
  g_assert_no_error (error);
  if (call_count == 4)
    g_main_loop_quit (fixture->mainloop);
}

static void
test_sender_async_on_failure_save_payload_to_file (struct SenderFixture *fixture,
                                                   gconstpointer         unused)
{
  GVariant *first = create_payload ("foo", 2001, TRUE);
  GVariant *second = create_payload ("bar", 2002, FALSE);
  GVariant *third = create_payload ("biz", 2003, TRUE);
  GVariant *fourth = create_payload ("baz", 2004, FALSE);

  fixture->connection->_web_send_sync_func = mock_web_send_sometimes_fail_sync;
  fixture->connection->_web_send_async_func = mock_web_send_sometimes_fail_async;

  emtr_sender_send_data (fixture->test_object, first, NULL,
                         (GAsyncReadyCallback)async_on_failure_save_payload_to_file_done,
                         fixture);
  emtr_sender_send_data (fixture->test_object, second, NULL,
                         (GAsyncReadyCallback)async_on_failure_save_payload_to_file_done,
                         fixture);
  emtr_sender_send_data (fixture->test_object, third, NULL,
                         (GAsyncReadyCallback)async_on_failure_save_payload_to_file_done,
                         fixture);
  emtr_sender_send_data (fixture->test_object, fourth, NULL,
                         (GAsyncReadyCallback)async_on_failure_save_payload_to_file_done,
                         fixture);

  g_main_loop_run (fixture->mainloop);

  g_variant_unref (first);
  g_variant_unref (second);
  g_variant_unref (third);
  g_variant_unref (fourth);

  gchar *loaded_payload = get_payload_from_file (fixture->test_object);
  /* This is a bit iffy because the order is not necessarily guaranteed */
  g_assert_cmpstr (loaded_payload, ==, EXPECTED_DATA_QUEUE);
  g_free (loaded_payload);
}

static void
async_cancel_send_done (EmtrSender           *sender,
                        GAsyncResult         *result,
                        struct SenderFixture *fixture)
{
  GError *error = NULL;
  gboolean success = emtr_sender_send_data_finish (fixture->test_object, result,
                                                   &error);
  g_assert (!success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_error_free (error);

  g_main_loop_quit (fixture->mainloop);
}

static void
test_sender_async_cancel_send (struct SenderFixture *fixture,
                               gconstpointer         unused)
{
  GCancellable *cancellable = g_cancellable_new ();

  fixture->connection->_web_send_sync_func = mock_web_send_exception_sync;
  fixture->connection->_web_send_async_func = mock_web_send_exception_async;

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  emtr_sender_send_data (fixture->test_object, payload, cancellable,
                         (GAsyncReadyCallback)async_cancel_send_done,
                         fixture);
  g_cancellable_cancel (cancellable);
  g_main_loop_run (fixture->mainloop);

  g_variant_unref (payload);
  g_object_unref (cancellable);
}

static void
async_send_queue_done (EmtrSender           *sender,
                       GAsyncResult         *result,
                       struct SenderFixture *fixture)
{
  GError *error = NULL;
  gboolean success = emtr_sender_send_queued_data_finish (fixture->test_object,
                                                          result, &error);
  g_assert (success);
  g_assert_no_error (error);

  g_main_loop_quit (fixture->mainloop);
}

static void
test_sender_async_sends_all_data_in_queue (struct SenderFixture *fixture,
                                           gconstpointer         unused)
{
  ensure_mock_queue (fixture->storage_file);
  emtr_sender_send_queued_data (fixture->test_object, NULL,
                                (GAsyncReadyCallback)async_send_queue_done,
                                fixture);
  g_main_loop_run (fixture->mainloop);

  gchar *loaded_queue = get_payload_from_file (fixture->test_object);
  g_assert_cmpstr (loaded_queue, ==, "[]");
  g_free (loaded_queue);
}

static void
test_sender_async_requeues_data_that_still_cant_be_sent (struct SenderFixture *fixture,
                                                         gconstpointer         unused)
{
  ensure_mock_queue (fixture->storage_file);
  fixture->connection->_web_send_sync_func = mock_web_send_exception_sync;
  fixture->connection->_web_send_async_func = mock_web_send_exception_async;

  emtr_sender_send_queued_data (fixture->test_object, NULL,
                                (GAsyncReadyCallback)async_send_queue_done,
                                fixture);
  g_main_loop_run (fixture->mainloop);

  gchar *loaded_queue = get_payload_from_file (fixture->test_object);
  /* Again, a bit iffy because the order of the queue is not guaranteed */
  g_assert_cmpstr (loaded_queue, ==, MOCK_QUEUE);
  g_free (loaded_queue);
}

static void
async_send_data_done (EmtrSender           *sender,
                      GAsyncResult         *result,
                      struct SenderFixture *fixture)
{
  GError *error = NULL;
  gboolean success = emtr_sender_send_data_finish (fixture->test_object,
                                                   result, &error);
  g_assert (success);
  g_assert_no_error (error);

  g_main_loop_quit (fixture->mainloop);
}

static void
test_sender_async_send_data_deals_with_nonexistent_queue_dir (struct SenderFixture *fixture,
                                                              gconstpointer         unused)
{
  ensure_queue_dir_doesnt_exist (fixture->tmpdir);

  GVariant *payload = create_payload ("foo", 2001, TRUE);

  fixture->connection->_web_send_sync_func = mock_web_send_exception_sync;
  fixture->connection->_web_send_async_func = mock_web_send_exception_async;

  emtr_sender_send_data (fixture->test_object, payload, NULL,
                         (GAsyncReadyCallback)async_send_data_done,
                         fixture);

  g_main_loop_run (fixture->mainloop);

  /* Asserts success in async_send_data_done() */

  g_variant_unref (payload);

  gchar *loaded_queue = get_payload_from_file (fixture->test_object);
  g_assert_cmpstr (loaded_queue, ==, EXPECTED_CREATED_QUEUE);
  g_free (loaded_queue);
}

static void
test_sender_async_send_queued_data_deals_with_nonexistent_queue_dir (struct SenderFixture *fixture,
                                                                     gconstpointer         unused)
{
  ensure_queue_dir_doesnt_exist (fixture->tmpdir);

  emtr_sender_send_queued_data (fixture->test_object, NULL,
                                (GAsyncReadyCallback)async_send_queue_done,
                                fixture);
  g_main_loop_run (fixture->mainloop);

  /* Asserts success in async_send_queue_done() */
}

static void
test_sender_feedback_sends_correct_format (struct SenderFixture *fixture,
                                           gconstpointer         unused)
{
  /* Put a different object into the test fixture */
  g_object_unref (fixture->test_object);
  g_object_unref (fixture->connection);
  fixture->test_object = emtr_sender_new_for_feedback ();
  g_object_get (fixture->test_object,
                "connection", &fixture->connection,
                NULL);

  fixture->connection->_web_send_sync_func = mock_web_send_assert_feedback_sync;
  fixture->connection->_web_send_async_func =
    mock_web_send_assert_feedback_async;
  fixture->connection->_web_send_finish_func = mock_web_send_finish;

  GVariant *payload = create_payload ("foo", 1234, TRUE);
  g_assert (emtr_sender_send_data_sync (fixture->test_object, payload,
                                        NULL, NULL));
  g_assert (mock_web_send_assert_data_called);
  g_variant_unref (payload);

  /* More assertions in mock_web_send_assert_feedback_sync() */
}

void
add_sender_tests (void)
{
  g_test_add_func ("/sender/absolute-storage-path-is-unchanged",
                   test_sender_absolute_storage_path_is_unchanged);
  g_test_add_func ("/sender/relative-storage-path-is-interpreted",
                   test_sender_relative_storage_path_is_interpreted);
  g_test_add_func ("/sender/new-session-metrics-succeeds",
                   test_sender_new_session_metrics_succeeds);
  g_test_add_func ("/sender/new-app-metrics-succeeds",
                   test_sender_new_app_metrics_succeeds);
  g_test_add_func ("/sender/new-feedback-succeeds",
                   test_sender_new_feedback_succeeds);

#define ADD_SENDER_TEST_FUNC(path, func) \
  g_test_add ((path), struct SenderFixture, NULL, setup, (func), teardown)

  ADD_SENDER_TEST_FUNC ("/sender/sync/invoking-send-data",
                        test_sender_invoking_send_data);
  ADD_SENDER_TEST_FUNC ("/sender/sync/on-failure-save-payload-to-file",
                        test_sender_on_failure_save_payload_to_file);
  ADD_SENDER_TEST_FUNC ("/sender/sync/cancel-send", test_sender_cancel_send);
  ADD_SENDER_TEST_FUNC ("/sender/sync/sends-all-data-in-queue",
                        test_sender_sync_sends_all_data_in_queue);
  ADD_SENDER_TEST_FUNC ("/sender/sync/requeues-data-that-still-cant-be-sent",
                        test_sender_sync_requeues_data_that_still_cant_be_sent);
  ADD_SENDER_TEST_FUNC ("/sender/sync/send-data-deals-with-nonexistent-queue-dir",
                        test_sender_sync_send_data_deals_with_nonexistent_queue_dir);
  ADD_SENDER_TEST_FUNC ("/sender/sync/send-queued-data-deals-with-nonexistent-queue-dir",
                        test_sender_sync_send_queued_data_deals_with_nonexistent_queue_dir);
  ADD_SENDER_TEST_FUNC ("/sender/async/invoking-send-data",
                        test_sender_async_invoking_send_data);
  ADD_SENDER_TEST_FUNC ("/sender/async/on-failure-save-payload-to-file",
                        test_sender_async_on_failure_save_payload_to_file);
  ADD_SENDER_TEST_FUNC ("/sender/async/cancel-send",
                        test_sender_async_cancel_send);
  ADD_SENDER_TEST_FUNC ("/sender/async/sends-all-data-in-queue",
                        test_sender_async_sends_all_data_in_queue);
  ADD_SENDER_TEST_FUNC ("/sender/async/requeues-data-that-still-cant-be-sent",
                        test_sender_async_requeues_data_that_still_cant_be_sent);
  ADD_SENDER_TEST_FUNC ("/sender/async/send-data-deals-with-nonexistent-queue-dir",
                        test_sender_async_send_data_deals_with_nonexistent_queue_dir);
  ADD_SENDER_TEST_FUNC ("/sender/async/send-queued-data-deals-with-nonexistent-queue-dir",
                        test_sender_async_send_queued_data_deals_with_nonexistent_queue_dir);
  ADD_SENDER_TEST_FUNC ("/sender/feedback-sends-correct-format",
                        test_sender_feedback_sends_correct_format);

#undef ADD_SENDER_TEST_FUNC
}
