/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-web-private.h"

#include <string.h>
#include <glib.h>
#include <libsoup/soup.h>

/*
 * SECTION:web
 * Facility for sending and receiving online data
 */

struct AuthData {
  gchar *username;
  gchar *password;
};

static struct AuthData *
auth_data_new (const gchar *username,
               const gchar *password)
{
  struct AuthData *retval = g_slice_new0 (struct AuthData);
  retval->username = g_strdup (username);
  retval->password = g_strdup (password);
  return retval;
}

static void
auth_data_free (struct AuthData *data)
{
  g_free (data->username);
  g_free (data->password);
  g_slice_free (struct AuthData, data);
}

struct AsyncPostData {
  SoupSession *session;
  SoupMessage *message;
  struct AuthData *auth_data;
  gchar *uri; /* for error reporting */
};

static void
async_post_data_free (struct AsyncPostData *data)
{
  g_free (data->uri);
  auth_data_free (data->auth_data);
  g_object_unref (data->session);
  g_object_unref (data->message);
}

/* Handler for authenticating the HTTP POST */
static void
setup_authentication (SoupSession     *session,
                      SoupMessage     *request,
                      SoupAuth        *auth,
                      gboolean         retrying,
                      struct AuthData *data)
{
  /* If it didn't authenticate the first time, then we have nothing else to
  try */
  if (retrying)
    return;

  soup_auth_authenticate (auth, data->username, data->password);
}

static SoupSession *
prepare_session (struct AuthData *data)
{
  SoupSession *session = soup_session_new ();
  g_signal_connect (session, "authenticate",
                    G_CALLBACK (setup_authentication), data);
  return session;
}

static SoupMessage *
prepare_message (const gchar *uri,
                 const gchar *json_data)
{
  SoupMessage *request = soup_message_new (SOUP_METHOD_POST, uri);
  SoupMessageHeaders *headers;

  soup_message_set_request (request, "application/x-www-form-urlencoded",
                            SOUP_MEMORY_COPY, json_data, strlen (json_data));
  g_object_get (request,
                "request-headers", &headers,
                NULL);
  soup_message_headers_append (headers,
                               "Accept", "application/json");
  return request;
}

static gboolean
interpret_status_code (guint       status,
                       SoupMessage *message,
                       const gchar *uri,
                       GError     **error)
{
  if (status != 200)
    {
      gchar *reason;
      g_object_get (message,
                    "reason-phrase", &reason,
                    NULL);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not access URI '%s' using authentication. "
                   "HTTP status code %d, reason: %s.",
                   uri, status, reason);
      return FALSE;
    }
  return TRUE;
}

static void
queue_message_callback (SoupSession *session,
                        SoupMessage *message,
                        GTask *task)
{
  GError *error = NULL;
  guint status;
  g_object_get (message,
                "status-code", &status,
                NULL);
  struct AsyncPostData *data = g_task_get_task_data (task);
  gboolean success = interpret_status_code (status, message, data->uri, &error);
  if (success)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

/* "Public" internal API */

/*
 * emtr_web_post_authorized_sync:
 * @uri: URI to post to
 * @json_data: the JSON data to include in the post request body, converted to a
 * string
 * @username: the username to authorize with
 * @password: the password to authorize with
 * @cancellable: (allow-none): currently unused, pass %NULL
 * @error: return location for an error, or %NULL
 *
 * Synchronously carries out an HTTP POST request at the URI specified by @uri.
 * The request body is @json_data.
 * The credentials @username and @password are used to authenticate the request.
 *
 * Returns: %TRUE on success, %FALSE otherwise, in which case @error is set.
 */
gboolean
emtr_web_post_authorized_sync (const gchar  *uri,
                               const gchar  *json_data,
                               const gchar  *username,
                               const gchar  *password,
                               GCancellable *cancellable,
                               GError      **error)
{
  struct AuthData *data = auth_data_new (username, password);
  SoupSession *session = prepare_session (data);
  SoupMessage *request = prepare_message (uri, json_data);

  guint status = soup_session_send_message (session, request);
  gboolean success = interpret_status_code (status, request, uri, error);

  g_object_unref (session);
  auth_data_free (data);

  return success;
}

/*
 * emtr_web_post_authorized:
 * @uri: URI to post to
 * @json_data: the JSON data to include in the post request body, converted to a
 * string
 * @username: the username to authorize with
 * @password: the password to authorize with
 * @cancellable: (allow-none): not used (for future expansion)
 * @callback: (scope async): function to call when the post is finished
 * @user_data: (closure): extra parameter to pass to @callback
 *
 * Starts an asynchronous HTTP POST request at the URI specified by @uri.
 * The request body is @json_data.
 * The credentials @username and @password are used to authenticate the request.
 *
 * When the post is finished, @callback will be called.
 * You can then call emtr_web_post_authorized_finish() to get the result of the
 * operation.
 */
void
emtr_web_post_authorized (const gchar        *uri,
                          const gchar        *json_data,
                          const gchar        *username,
                          const gchar        *password,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
  struct AsyncPostData *data = g_slice_new0 (struct AsyncPostData);
  data->auth_data = auth_data_new (username, password);
  data->session = prepare_session (data->auth_data);
  data->message = prepare_message (uri, json_data);
  data->uri = g_strdup (uri);
  GTask *task = g_task_new (NULL, NULL, callback, user_data);
  g_task_set_task_data (task, data, (GDestroyNotify)async_post_data_free);
  soup_session_queue_message (data->session, data->message,
                              (SoupSessionCallback)queue_message_callback,
                              task);
}

/*
 * emtr_web_post_authorized_finish:
 * @result: a #GAsyncResult
 * @error: return location for an error, or %NULL
 *
 * Finishes an operation begun by emtr_web_post_authorized().
 *
 * Returns: %TRUE on success, %FALSE otherwise, in which case @error is set.
 */
gboolean
emtr_web_post_authorized_finish (GAsyncResult *result,
                                 GError      **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
