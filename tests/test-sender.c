/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <string.h>
#include <glib.h>

#include <eosmetrics/emtr-sender.h>
#include <eosmetrics/emtr-connection.h>
#include "run-tests.h"

#define TMP_DIRECTORY_TEMPLATE "/tmp/metricssendprocesstestXXXXXX"
#define TMP_FILE_TEMPLATE "metricssendprocesstestXXXXXX.json"
#define EXPECTED_RELATIVE_FILENAME "metricssendprocesstest.json"
#define EXPECTED_DATA_QUEUE "[{\"message\":\"bar\",\"timestamp\":2002,\"bug\":false},{\"message\":\"biz\",\"timestamp\":2003,\"bug\":true}]"

static gboolean mock_web_send_assert_data_called = FALSE;

static gboolean
mock_web_send_sometimes_fail (const gchar *uri,
                              const gchar *data,
                              const gchar *username,
                              const gchar *password,
                              GError     **error)
{
  static unsigned times_called = 0;
  ++times_called;
  if (times_called % 4 == 0 || times_called % 4 == 1)
    return mock_web_send (uri, data, username, password, error);
  return mock_web_send_exception (uri, data, username, password, error);
}

static gboolean
mock_web_send_assert_data (const gchar *uri,
                           const gchar *data,
                           const gchar *username,
                           const gchar *password,
                           GError     **error)
{
  mock_web_send_assert_data_called = TRUE;
  g_assert (strstr (data, "\"message\":\"foo bar\""));
  g_assert (strstr (data, "\"timestamp\":1001"));
  g_assert (strstr (data, "\"bug\":false"));
  return TRUE;
}

struct SenderFixture {
  GFile *tmpdir;
  GFile *storage_file;
  GFile *fingerprint_file;

  EmtrConnection *connection;
  EmtrSender *test_object;
};

static void
setup (struct SenderFixture *fixture,
       gconstpointer         unused)
{
  gchar *tmpdirpath = g_mkdtemp (g_strdup (TMP_DIRECTORY_TEMPLATE));
  fixture->tmpdir = g_file_new_for_path (tmpdirpath);
  fixture->storage_file = g_file_get_child (fixture->tmpdir, "data.json");
  fixture->fingerprint_file = g_file_get_child (fixture->tmpdir, "fingerprint");
  g_free (tmpdirpath);

  fixture->connection = g_object_new (EMTR_TYPE_CONNECTION,
                                      "fingerprint-file", fixture->fingerprint_file,
                                      NULL);
  fixture->connection->_web_send_func = mock_web_send;

  fixture->test_object = g_object_new (EMTR_TYPE_SENDER,
                                       "storage-file", fixture->storage_file,
                                       "connection", fixture->connection,
                                       NULL);
}

static void
teardown (struct SenderFixture *fixture,
          gconstpointer         unused)
{
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
test_sender_invoking_send_data (struct SenderFixture *fixture,
                                gconstpointer         unused)
{
  GError *error = NULL;
  GVariant *payload = create_payload ("foo bar", 1001, FALSE);

  fixture->connection->_web_send_func = mock_web_send_assert_data;

  g_assert (emtr_sender_send_data (fixture->test_object, payload, &error));
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

  fixture->connection->_web_send_func = mock_web_send_sometimes_fail;

  g_assert (emtr_sender_send_data (fixture->test_object, first, &error));
  g_assert_no_error (error);
  g_variant_unref (first);
  g_assert (emtr_sender_send_data (fixture->test_object, second, &error));
  g_assert_no_error (error);
  g_variant_unref (second);
  g_assert (emtr_sender_send_data (fixture->test_object, third, &error));
  g_assert_no_error (error);
  g_variant_unref (third);
  g_assert (emtr_sender_send_data (fixture->test_object, fourth, &error));
  g_assert_no_error (error);
  g_variant_unref (fourth);

  gchar *loaded_payload = get_payload_from_file (fixture->test_object);
  g_assert_cmpstr (loaded_payload, ==, EXPECTED_DATA_QUEUE);
  g_free (loaded_payload);
}

void
add_sender_tests (void)
{
  g_test_add_func ("/sender/absolute-storage-path-is-unchanged",
                   test_sender_absolute_storage_path_is_unchanged);
  g_test_add_func ("/sender/relative-storage-path-is-interpreted",
                   test_sender_relative_storage_path_is_interpreted);

#define ADD_SENDER_TEST_FUNC(path, func) \
  g_test_add ((path), struct SenderFixture, NULL, setup, (func), teardown)

  ADD_SENDER_TEST_FUNC ("/sender/invoking-send-data",
                        test_sender_invoking_send_data);
  ADD_SENDER_TEST_FUNC ("/sender/on-failure-save-payload-to-file",
                        test_sender_on_failure_save_payload_to_file);

#undef ADD_SENDER_TEST_FUNC
}
