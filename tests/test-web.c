/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <eosmetrics/emtr-web-private.h>
#include "run-tests.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>

#define EXPECTED_USERNAME "endlessos"
#define EXPECTED_PASSWORD "sosseldne"
#define EXPECTED_JSON_DATA "{\n" \
                           "  \"string\": \"hello\",\n" \
                           "  \"int\": 5\n" \
                           "}"

typedef struct _WebFixture WebFixture;
typedef void (*WebTestFunc)(WebFixture *);
typedef void (*WebHandlerFunc)(SoupServer *, SoupMessage *, const char *,
                               GHashTable *, SoupClientContext *, WebFixture *);
struct _WebFixture {
  /* Test HTTP server */
  SoupServer *server;
  SoupAuthDomain *auth_domain;

  /* Stuff for running the server in another thread */
  GMainContext *mainctxt;
  GMainLoop *mainloop;
  GThread *server_thread;

  /* The server handler function that the server should handle a particular test
  request with */
  WebHandlerFunc test_handler;
};

/* Authentication callback for test server */
static gboolean
server_auth (SoupAuthDomain *auth,
             SoupMessage    *message,
             const char     *username,
             const char     *password,
             WebFixture     *fixture)
{
  return (strcmp (username, EXPECTED_USERNAME) == 0
          && strcmp (password, EXPECTED_PASSWORD) == 0);
}

/* URI handler callback for test server. This just delegates to another handler
provided with the unit test. */
static void
server_handler (SoupServer        *server,
                SoupMessage       *message,
                const char        *path,
                GHashTable        *query,
                SoupClientContext *client,
                WebFixture        *fixture)
{
  fixture->test_handler (server, message, path, query, client, fixture);
}

/* Unit test setup function. Starts a simple HTTP server in another thread, that
the library functions can post to. */
static void
setup (WebFixture   *fixture,
       gconstpointer data)
{
  fixture->mainctxt = g_main_context_new ();
  fixture->mainloop = g_main_loop_new (fixture->mainctxt, FALSE);

  fixture->server = soup_server_new ("port", 8123,
                                     "async-context", fixture->mainctxt,
                                     NULL);
  fixture->auth_domain = soup_auth_domain_basic_new ("realm", "Test Realm",
                                                     "auth-callback", server_auth,
                                                     "auth-data", fixture,
                                                     "add-path", "/",
                                                     NULL);
  soup_server_add_auth_domain (fixture->server, fixture->auth_domain);

  fixture->test_handler = (WebHandlerFunc)data;
  soup_server_add_handler (fixture->server, "/",
                           (SoupServerCallback)server_handler,
                           fixture, NULL);

  soup_server_run_async (fixture->server);
  fixture->server_thread = g_thread_new ("test server",
                                         (GThreadFunc)g_main_loop_run,
                                         fixture->mainloop);
}

/* Unit test teardown function. The server should have exited by now, so wait
for the thread to finish. */
static void
teardown (WebFixture   *fixture,
          gconstpointer unused)
{
  soup_server_quit (fixture->server);
  g_main_loop_quit (fixture->mainloop);
  g_thread_join (fixture->server_thread);
  g_object_unref (fixture->server);
  g_thread_unref (fixture->server_thread);
  g_main_loop_unref (fixture->mainloop);
  g_main_context_unref (fixture->mainctxt);
}

/* Handler that accepts all requests */
static void
okay_everything_handler (SoupServer        *server,
                         SoupMessage       *message,
                         const char        *path,
                         GHashTable        *query,
                         SoupClientContext *client,
                         WebFixture        *fixture)
{
  soup_message_set_status (message, 200);
}

/* Handler that returns 404 on all requests */
static void
reject_everything_handler (SoupServer        *server,
                           SoupMessage       *message,
                           const char        *path,
                           GHashTable        *query,
                           SoupClientContext *client,
                           WebFixture        *fixture)
{
  soup_message_set_status (message, 404);
}

/* Handler that sets data blobs on the server object for the test to read */
static void
analyze_request_handler (SoupServer        *server,
                         SoupMessage       *message,
                         const char        *path,
                         GHashTable        *query,
                         SoupClientContext *client,
                         WebFixture        *fixture)
{
  SoupMessageHeaders *headers;
  SoupMessageBody *body;
  g_object_get (message,
                "request-headers", &headers,
                "request-body", &body,
                NULL);

  g_object_set_data (G_OBJECT (server), "content-type",
                     g_strdup (soup_message_headers_get_list (headers,
                                                              "Content-Type")));
  g_object_set_data (G_OBJECT (server), "accept",
                     g_strdup (soup_message_headers_get_list (headers,
                                                              "Accept")));
  soup_message_headers_free (headers);

  SoupBuffer *buffer = soup_message_body_flatten (body);
  GBytes *bytes = soup_buffer_get_as_bytes (buffer);
  soup_message_body_free (body);
  soup_buffer_free (buffer);

  gsize length;
  const gchar *bytes_data = g_bytes_get_data (bytes, &length);
  gchar *message_body = g_strndup (bytes_data, length);
  g_bytes_unref (bytes);
  g_object_set_data (G_OBJECT (server), "body", message_body);

  soup_message_set_status (message, 200);
}

static void
test_web_post_authorized_success (WebFixture   *fixture,
                                  gconstpointer unused)
{
  GError *error = NULL;
  gboolean success = emtr_web_post_authorized ("http://localhost:8123", "{}",
                                               EXPECTED_USERNAME,
                                               EXPECTED_PASSWORD,
                                               NULL,
                                               &error);
  g_assert (success);
  g_assert_no_error (error);
}

static void
test_web_post_fails_on_404 (WebFixture   *fixture,
                            gconstpointer unused)
{
  GError *error = NULL;
  gboolean success = emtr_web_post_authorized ("http://localhost:8123", "{}",
                                               EXPECTED_USERNAME,
                                               EXPECTED_PASSWORD,
                                               NULL,
                                               &error);
  g_assert (!success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert (strstr (error->message, "404") != NULL);
}

static void
test_web_post_fails_on_wrong_credentials (WebFixture   *fixture,
                                          gconstpointer unused)
{
  GError *error = NULL;
  gboolean success = emtr_web_post_authorized ("http://localhost:8123", "{}",
                                               "fake-username",
                                               "fake-password",
                                               NULL,
                                               &error);
  g_assert (!success);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert (strstr (error->message, "401") != NULL);
}

static void
test_web_ensure_all_data_sent_correctly (WebFixture   *fixture,
                                         gconstpointer unused)
{
  GError *error = NULL;
  gboolean success = emtr_web_post_authorized ("http://localhost:8123",
                                               EXPECTED_JSON_DATA,
                                               EXPECTED_USERNAME,
                                               EXPECTED_PASSWORD,
                                               NULL,
                                               &error);
  g_assert (success);
  g_assert_no_error (error);

  gchar *content_type = g_object_get_data (G_OBJECT (fixture->server),
                                           "content-type");
  g_assert_cmpstr (content_type, ==, "application/x-www-form-urlencoded");
  g_free (content_type);

  gchar *accept = g_object_get_data (G_OBJECT (fixture->server), "accept");
  g_assert_cmpstr (accept, ==, "application/json");
  g_free (accept);

  gchar *body = g_object_get_data (G_OBJECT (fixture->server), "body");
  g_assert_cmpstr (body, ==, EXPECTED_JSON_DATA);
  g_free (body);
}

void
add_web_tests (void)
{
#define ADD_WEB_TEST_FUNC(path, func, handler) \
  g_test_add ((path), WebFixture, (handler), setup, (func), teardown)

  ADD_WEB_TEST_FUNC ("/web/post-authorized-success",
                     test_web_post_authorized_success,
                     okay_everything_handler);
  ADD_WEB_TEST_FUNC ("/web/post-fails-on-404",
                     test_web_post_fails_on_404,
                     reject_everything_handler);
  ADD_WEB_TEST_FUNC ("/web/post-fails-on-wrong-credentials",
                     test_web_post_fails_on_wrong_credentials,
                     okay_everything_handler);
  ADD_WEB_TEST_FUNC ("/web/ensure-all-data-sent-correctly",
                     test_web_ensure_all_data_sent_correctly,
                     analyze_request_handler);

#undef ADD_WEB_TEST_FUNC
}
