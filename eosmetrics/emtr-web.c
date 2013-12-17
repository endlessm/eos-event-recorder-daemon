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

/*
 * emtr_web_post_authorized:
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
emtr_web_post_authorized (const gchar  *uri,
                          const gchar  *json_data,
                          const gchar  *username,
                          const gchar  *password,
                          GCancellable *cancellable,
                          GError      **error)
{
  SoupSession *session = soup_session_new ();
  struct AuthData *data = auth_data_new (username, password);

  g_signal_connect (session, "authenticate",
                    G_CALLBACK (setup_authentication), data);

  SoupMessage *request = soup_message_new (SOUP_METHOD_POST, uri);
  SoupMessageHeaders *headers;

  soup_message_set_request (request, "application/x-www-form-urlencoded",
                            SOUP_MEMORY_COPY, json_data, strlen (json_data));
  g_object_get (request,
                "request-headers", &headers,
                NULL);
  soup_message_headers_append (headers,
                               "Accept", "application/json");

  guint status = soup_session_send_message (session, request);

  if (status != 200)
    {
      gchar *reason;
      g_object_get (request,
                    "reason-phrase", &reason,
                    NULL);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not access URI '%s' using authentication. "
                   "HTTP status code %d, reason: %s.",
                   uri, status, reason);
    }

  g_object_unref (session);
  auth_data_free (data);

  return (status == 200);
}
