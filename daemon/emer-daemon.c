/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014, 2015 Endless Mobile, Inc. */

/*
 * This file is part of eos-event-recorder-daemon.
 *
 * eos-event-recorder-daemon is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 *
 * eos-event-recorder-daemon is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with eos-event-recorder-daemon.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <time.h>
#include <uuid/uuid.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include <eosmetrics/eosmetrics.h>

#include "emer-daemon.h"
#include "emer-machine-id-provider.h"
#include "emer-network-send-provider.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"
#include "shared/metrics-util.h"

/*
 * The version of this client's network protocol.
 */
#define CLIENT_VERSION_NUMBER "2"

/*
 * The minimum number of seconds to wait before attempting the first retry of a
 * network request that failed with a non-fatal error.
 */
#define INITIAL_BACKOFF_SEC 1

/*
 * The maximum number of attempts to upload a particular batch of metric events
 * before giving up and dropping them.
 */
#define NETWORK_ATTEMPT_LIMIT 8

/*
 * How many seconds to delay between trying to send metrics to the proxy server
 * if we are online, or to the persistent cache, if we are offline.
 *
 * For QA, the "dev" environment delay is much shorter.
 */
#define DEV_NETWORK_SEND_INTERVAL (60u * 15u) // Fifteen minutes
#define PRODUCTION_NETWORK_SEND_INTERVAL (60u * 60u) // One hour

#define WHAT "shutdown"
#define WHO "EndlessOS Event Recorder Daemon"
#define WHY "Flushing events to disk"
#define MODE "delay"
#define INHIBIT_ARGS "('" WHAT "', '" WHO "', '" WHY "', '" MODE "')"

#define EVENT_VALUE_TYPE_STRING "(xmv)"
#define EVENT_VALUE_ARRAY_TYPE_STRING "a" EVENT_VALUE_TYPE_STRING
#define EVENT_VALUE_ARRAY_TYPE G_VARIANT_TYPE (EVENT_VALUE_ARRAY_TYPE_STRING)

#define SINGULAR_TYPE_STRING "(uayxmv)"
#define AGGREGATE_TYPE_STRING "(uayxxmv)"
#define SEQUENCE_TYPE_STRING "(uay" EVENT_VALUE_ARRAY_TYPE_STRING ")"

#define SINGULAR_TYPE G_VARIANT_TYPE (SINGULAR_TYPE_STRING)
#define AGGREGATE_TYPE G_VARIANT_TYPE (AGGREGATE_TYPE_STRING)
#define SEQUENCE_TYPE G_VARIANT_TYPE (SEQUENCE_TYPE_STRING)

#define SINGULAR_ARRAY_TYPE_STRING "a" SINGULAR_TYPE_STRING
#define AGGREGATE_ARRAY_TYPE_STRING "a" AGGREGATE_TYPE_STRING
#define SEQUENCE_ARRAY_TYPE_STRING "a" SEQUENCE_TYPE_STRING

#define SINGULAR_ARRAY_TYPE G_VARIANT_TYPE (SINGULAR_ARRAY_TYPE_STRING)
#define AGGREGATE_ARRAY_TYPE G_VARIANT_TYPE (AGGREGATE_ARRAY_TYPE_STRING)
#define SEQUENCE_ARRAY_TYPE G_VARIANT_TYPE (SEQUENCE_ARRAY_TYPE_STRING)

#define REQUEST_TYPE_STRING "(ixxay" SINGULAR_ARRAY_TYPE_STRING \
  AGGREGATE_ARRAY_TYPE_STRING SEQUENCE_ARRAY_TYPE_STRING ")"

#define RETRY_TYPE_STRING "(ixx@ay@" SINGULAR_ARRAY_TYPE_STRING "@" \
  AGGREGATE_ARRAY_TYPE_STRING "@" SEQUENCE_ARRAY_TYPE_STRING ")"

#define MAX_REQUEST_PAYLOAD 100000 /* 100 kB */

#define METRICS_DISABLED_MESSAGE "Could not upload events because the " \
  "metrics system is disabled. You may enable the metrics system via " \
  "Settings > Privacy > Metrics."

#define UPLOADING_DISABLED_MESSAGE "Could not upload events because " \
  "uploading is disabled. You may enable uploading by setting " \
  "uploading_enabled to true in " PERMISSIONS_FILE "."

typedef struct _NetworkCallbackData
{
  EmerDaemon *daemon;
  GVariant *request_body;
  guint64 token;
  gsize num_buffer_events;
  gint attempt_num;
  GTask *upload_task;
} NetworkCallbackData;

typedef struct _EmerDaemonPrivate
{
  gint shutdown_inhibitor;
  GDBusProxy *login_manager_proxy;

  guint network_send_interval;
  GQueue *upload_queue;
  gboolean uploading;

  SoupSession *http_session;

  GPtrArray *variant_array;
  gsize num_bytes_buffered;
  gboolean have_logged_overflow;

  /* Private storage for public properties */

  GRand *rand;

  gboolean use_default_server_uri;
  gchar *server_uri;

  guint upload_events_timeout_source_id;

  EmerMachineIdProvider *machine_id_provider;
  EmerNetworkSendProvider *network_send_provider;
  EmerPermissionsProvider *permissions_provider;

  EmerPersistentCache *persistent_cache;

  gboolean recording_enabled;

  gsize max_bytes_buffered;
} EmerDaemonPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerDaemon, emer_daemon, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_RANDOM_NUMBER_GENERATOR,
  PROP_SERVER_URI,
  PROP_NETWORK_SEND_INTERVAL,
  PROP_MACHINE_ID_PROVIDER,
  PROP_NETWORK_SEND_PROVIDER,
  PROP_PERMISSIONS_PROVIDER,
  PROP_PERSISTENT_CACHE,
  PROP_MAX_BYTES_BUFFERED,
  NPROPS
};

static GParamSpec *emer_daemon_props[NPROPS] = { NULL, };

enum
{
  SIGNAL_0,
  SIGNAL_UPLOAD_FINISHED,
  NSIGNALS
};

static guint emer_daemon_signals[NSIGNALS] = { 0u, };

static gboolean handle_upload_timer (EmerDaemon *self);

static void
finish_network_callback (NetworkCallbackData *callback_data)
{
  EmerDaemon *self = callback_data->daemon;
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  g_variant_unref (callback_data->request_body);
  g_object_unref (callback_data->upload_task);
  g_free (callback_data);

  priv->uploading = FALSE;
  g_signal_emit (self, emer_daemon_signals[SIGNAL_UPLOAD_FINISHED], 0u);
}

static void
release_shutdown_inhibitor (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (priv->shutdown_inhibitor != -1)
    {
      // We are currently holding a shutdown inhibitor.
      GError *error = NULL;
      gboolean released_shutdown_inhibitor =
        g_close (priv->shutdown_inhibitor, &error);
      if (!released_shutdown_inhibitor)
        {
          g_warning ("Could not release shutdown inhibitor: %s.",
                     error->message);
          g_error_free (error);
        }

      priv->shutdown_inhibitor = -1;
    }
}

static gboolean
is_uuid (GVariant *variant)
{
  const GVariantType *variant_type = g_variant_get_type (variant);
  if (!g_variant_type_equal (variant_type, G_VARIANT_TYPE_BYTESTRING))
    return FALSE;

  return g_variant_n_children (variant) == UUID_LENGTH;
}

static void
buffer_event (EmerDaemon *self,
              GVariant   *event)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  g_variant_ref_sink (event);
  gsize event_cost = emer_persistent_cache_cost (event);
  gsize new_bytes_buffered = priv->num_bytes_buffered + event_cost;
  if (event_cost > MAX_REQUEST_PAYLOAD || /* Don't get wedged by large event. */
      new_bytes_buffered > priv->max_bytes_buffered)
    {
      g_variant_unref (event);
      if (event_cost > MAX_REQUEST_PAYLOAD)
        {
          g_warning ("Dropping %" G_GSIZE_FORMAT "-byte event. The maximum "
                     "permissable event size (including type string with "
                     "null-terminating byte) is %d bytes.",
                     event_cost, MAX_REQUEST_PAYLOAD);
        }
      else if (!priv->have_logged_overflow)
        {
          g_warning ("The event buffer overflowed for the first time in the "
                     "life of this event recorder daemon. The maximum number "
                     "of bytes that may be buffered is %" G_GSIZE_FORMAT ".",
                     priv->max_bytes_buffered);
          priv->have_logged_overflow = TRUE;
        }
      return;
    }

  g_ptr_array_add (priv->variant_array, event);
  priv->num_bytes_buffered = new_bytes_buffered;
}

static void
remove_events (EmerDaemon *self,
               gsize       num_events)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (num_events == 0)
    return;

  for (gint i = 0; i < num_events; i++)
    {
      GVariant *curr_variant = g_ptr_array_index (priv->variant_array, i);
      priv->num_bytes_buffered -= emer_persistent_cache_cost (curr_variant);
    }

  g_ptr_array_remove_range (priv->variant_array, 0u, num_events);
}

static void
flush_to_persistent_cache (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (!priv->recording_enabled)
    return;

  gsize num_events_stored;
  GError *error = NULL;
  gboolean store_succeeded =
    emer_persistent_cache_store (priv->persistent_cache,
                                 (GVariant **) priv->variant_array->pdata,
                                 priv->variant_array->len,
                                 &num_events_stored,
                                 &error);
  if (!store_succeeded)
    {
      g_warning ("Failed to flush buffer to persistent cache: %s.",
                 error->message);
      g_error_free (error);
      return;
    }

  remove_events (self, num_events_stored);
}

static void
remove_from_persistent_cache (EmerDaemon *self,
                              guint64     token)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GError *error = NULL;
  gboolean remove_succeeded =
    emer_persistent_cache_remove (priv->persistent_cache, token, &error);
  if (!remove_succeeded)
    {
      g_warning ("Failed to remove events from persistent cache with token %"
                 G_GUINT64_FORMAT ". They may be resent to the server. Error: "
                 "%s.", token, error->message);
      g_error_free (error);
    }
}

static void
backoff (GRand *rand,
         gint   attempt_num)
{
  gulong base_backoff_sec = INITIAL_BACKOFF_SEC;
  for (gint i = 0; i < attempt_num - 1; i++)
    base_backoff_sec *= 2;

  gdouble random_factor = g_rand_double_range (rand, 1, 2);
  gdouble randomized_backoff_sec = random_factor * (gdouble) base_backoff_sec;
  gulong randomized_backoff_usec =
    (gulong) (G_USEC_PER_SEC * randomized_backoff_sec);
  g_usleep (randomized_backoff_usec);
}

/*
 * Returned object is owned by calling code. Free with soup_uri_free() when
 * done.
 */
static SoupURI *
get_http_request_uri (EmerDaemon   *self,
                      const guchar *data,
                      gsize         length)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  gchar *checksum =
    g_compute_checksum_for_data (G_CHECKSUM_SHA512, data, length);
  gchar *http_request_uri_string =
    g_build_filename (priv->server_uri, checksum, NULL);
  g_free (checksum);

  SoupURI *http_request_uri = soup_uri_new (http_request_uri_string);

  if (http_request_uri == NULL)
    g_error ("Invalid URI: %s.", http_request_uri_string);

  g_free (http_request_uri_string);

  return http_request_uri;
}

/*
 * Sets an absolute timestamp and a boot-offset-corrected relative timestamp in
 * the out parameters. Returns FALSE on failure and TRUE on success. The values
 * of the out parameters are undefined on failure.
 */
static gboolean
get_offset_timestamps (EmerDaemon *self,
                       gint64     *rel_timestamp_ptr,
                       gint64     *abs_timestamp_ptr,
                       GError    **error)
{

  if (!emtr_util_get_current_time (CLOCK_BOOTTIME, rel_timestamp_ptr) ||
      !emtr_util_get_current_time (CLOCK_REALTIME, abs_timestamp_ptr))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not get current time.");
      return FALSE;
    }

  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  gint64 boot_offset;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, FALSE, error))
    return FALSE;

  *rel_timestamp_ptr += boot_offset;
  return TRUE;
}

static GVariant *
get_updated_request_body (EmerDaemon *self,
                          GVariant   *request_body,
                          GError    **error)
{
  gint32 send_number;
  GVariant *machine_id, *singulars, *aggregates, *sequences;
  g_variant_get (request_body, RETRY_TYPE_STRING, &send_number,
                 NULL /* relative time */, NULL /* absolute time */,
                 &machine_id, &singulars, &aggregates, &sequences);

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_offset_timestamps (self, &relative_timestamp, &absolute_timestamp,
                              error))
    return NULL;

  gint64 little_endian_relative_timestamp =
    swap_bytes_64_if_big_endian (relative_timestamp);
  gint64 little_endian_absolute_timestamp =
    swap_bytes_64_if_big_endian (absolute_timestamp);

  return g_variant_new (RETRY_TYPE_STRING, send_number,
                        little_endian_relative_timestamp,
                        little_endian_absolute_timestamp,
                        machine_id, singulars, aggregates, sequences);
}

// Handles HTTP or HTTPS responses.
static void
handle_http_response (SoupSession         *http_session,
                      SoupMessage         *http_message,
                      NetworkCallbackData *callback_data)
{
  EmerDaemon *self = callback_data->daemon;
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  guint status_code;
  g_object_get (http_message, "status-code", &status_code, NULL);
  if (SOUP_STATUS_IS_SUCCESSFUL (status_code))
    {
      g_task_return_boolean (callback_data->upload_task, TRUE);
      remove_from_persistent_cache (self, callback_data->token);
      remove_events (self, callback_data->num_buffer_events);
      flush_to_persistent_cache (self);
      finish_network_callback (callback_data);
      return;
    }

  gchar *reason_phrase;
  g_object_get (http_message, "reason-phrase", &reason_phrase, NULL);
  g_warning ("Attempt to upload metrics failed: %s.", reason_phrase);
  g_free (reason_phrase);

  if (++callback_data->attempt_num >= NETWORK_ATTEMPT_LIMIT)
    {
      g_task_return_new_error (callback_data->upload_task, G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "Maximum number of network attempts (%d) "
                               "reached.", NETWORK_ATTEMPT_LIMIT);
      finish_network_callback (callback_data);
      return;
    }

  if (SOUP_STATUS_IS_TRANSPORT_ERROR (status_code) ||
      SOUP_STATUS_IS_CLIENT_ERROR (status_code) ||
      SOUP_STATUS_IS_SERVER_ERROR (status_code))
    {
      backoff (priv->rand, callback_data->attempt_num);

      GError *error = NULL;
      GVariant *updated_request_body =
        get_updated_request_body (self, callback_data->request_body, &error);
      if (updated_request_body == NULL)
        {
          g_task_return_error (callback_data->upload_task, error);
          finish_network_callback (callback_data);
          return;
        }

      g_variant_unref (callback_data->request_body);
      callback_data->request_body = updated_request_body;

      gconstpointer serialized_request_body =
        g_variant_get_data (updated_request_body);
      if (serialized_request_body == NULL)
        {
          g_task_return_new_error (callback_data->upload_task, G_IO_ERROR,
                                   G_IO_ERROR_INVALID_DATA,
                                   "Could not serialize updated network "
                                   "request body.");
          finish_network_callback (callback_data);
          return;
        }

      gsize request_body_length = g_variant_get_size (updated_request_body);

      SoupURI *http_request_uri =
        get_http_request_uri (callback_data->daemon, serialized_request_body,
                              request_body_length);
      SoupMessage *new_http_message =
        soup_message_new_from_uri ("PUT", http_request_uri);
      soup_uri_free (http_request_uri);

      soup_message_set_request (new_http_message, "application/octet-stream",
                                SOUP_MEMORY_TEMPORARY, serialized_request_body,
                                request_body_length);

      soup_session_queue_message (http_session, new_http_message,
                                  (SoupSessionCallback) handle_http_response,
                                  callback_data);
      /* Old message is unreffed automatically, because it is not requeued. */

      return;
    }

  g_task_return_new_error (callback_data->upload_task, G_IO_ERROR,
                           G_IO_ERROR_FAILED, "Received HTTP status code: %u.",
                           status_code);
  finish_network_callback (callback_data);
}

static void
add_events_to_builders (GVariant       **events,
                        gsize            num_events,
                        GVariantBuilder *singulars,
                        GVariantBuilder *aggregates,
                        GVariantBuilder *sequences)
{
  for (gsize i = 0; i < num_events; i++)
    {
      GVariant *curr_event = events[i];
      const GVariantType *event_type = g_variant_get_type (curr_event);
      if (g_variant_type_equal (event_type, SINGULAR_TYPE))
        g_variant_builder_add_value (singulars, curr_event);
      else if (g_variant_type_equal (event_type, AGGREGATE_TYPE))
        g_variant_builder_add_value (aggregates, curr_event);
      else if (g_variant_type_equal (event_type, SEQUENCE_TYPE))
        g_variant_builder_add_value (sequences, curr_event);
      else
        g_error ("An event has an unexpected variant type.");
    }
}

/* Populates the given variant builders with at most MAX_REQUEST_PAYLOAD bytes
 * of data from the persistent cache. Returns TRUE if the current network
 * request should also include data from the in-memory buffer and FALSE
 * otherwise. Sets read_bytes to the number of bytes of data that were read and
 * token to a value that can be passed to emer_persistent_cache_remove to remove
 * the events that were added to the variant builders from the persistent cache.
 */
static gboolean
add_stored_events_to_builders (EmerDaemon        *self,
                               gsize             *read_bytes,
                               guint64           *token,
                               GVariantBuilder   *singulars,
                               GVariantBuilder   *aggregates,
                               GVariantBuilder   *sequences)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GVariant **variants;
  gsize num_variants;
  GError *error = NULL;
  gboolean read_succeeded =
    emer_persistent_cache_read (priv->persistent_cache, &variants,
                                MAX_REQUEST_PAYLOAD, &num_variants, token,
                                &error);
  if (!read_succeeded)
    {
      g_warning ("Could not read from persistent cache: %s.", error->message);
      g_error_free (error);
      *read_bytes = 0;
      *token = 0;
      return TRUE;
    }

  add_events_to_builders (variants, num_variants,
                          singulars, aggregates, sequences);

  gsize curr_read_bytes = 0;
  for (gsize i = 0; i < num_variants; i++)
    curr_read_bytes += emer_persistent_cache_cost (variants[i]);

  g_free (variants);

  *read_bytes = curr_read_bytes;

  return !emer_persistent_cache_has_more (priv->persistent_cache, *token);
}

static void
add_buffered_events_to_builders (EmerDaemon      *self,
                                 gsize            num_bytes,
                                 gsize           *num_variants,
                                 GVariantBuilder *singulars,
                                 GVariantBuilder *aggregates,
                                 GVariantBuilder *sequences)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  gsize curr_bytes = 0, curr_num_variants = 0;
  for (; curr_num_variants < priv->variant_array->len; curr_num_variants++)
    {
      GVariant *curr_event =
        g_ptr_array_index (priv->variant_array, curr_num_variants);
      curr_bytes += emer_persistent_cache_cost (curr_event);
      if (curr_bytes > num_bytes)
        break;
    }

  add_events_to_builders ((GVariant **) priv->variant_array->pdata,
                          curr_num_variants, singulars, aggregates, sequences);
  *num_variants = curr_num_variants;
}

static GVariant *
create_request_body (EmerDaemon *self,
                     guint64    *token,
                     gsize      *num_buffer_events,
                     GError    **error)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  uuid_t machine_id;
  gboolean read_id = emer_machine_id_provider_get_id (priv->machine_id_provider,
                                                      machine_id);
  if (!read_id)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not read machine ID.");
      return NULL;
    }

  GVariantBuilder machine_id_builder;
  get_uuid_builder (machine_id, &machine_id_builder);

  gint send_number =
    emer_network_send_provider_get_send_number (priv->network_send_provider);
  emer_network_send_provider_increment_send_number (priv->network_send_provider);

  GVariantBuilder singulars, aggregates, sequences;
  g_variant_builder_init (&singulars, SINGULAR_ARRAY_TYPE);
  g_variant_builder_init (&aggregates, AGGREGATE_ARRAY_TYPE);
  g_variant_builder_init (&sequences, SEQUENCE_ARRAY_TYPE);

  gsize num_bytes_read;
  gboolean add_from_buffer =
    add_stored_events_to_builders (self, &num_bytes_read, token,
                                   &singulars, &aggregates, &sequences);

  if (add_from_buffer)
    {
      gsize space_remaining = MAX_REQUEST_PAYLOAD - num_bytes_read;
      add_buffered_events_to_builders (self, space_remaining, num_buffer_events,
                                       &singulars, &aggregates, &sequences);
    }

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_offset_timestamps (self, &relative_timestamp, &absolute_timestamp,
                              error))
    return NULL;

  GVariant *request_body =
    g_variant_new (REQUEST_TYPE_STRING, send_number,
                   relative_timestamp, absolute_timestamp,
                   &machine_id_builder, &singulars, &aggregates, &sequences);

  g_variant_ref_sink (request_body);
  GVariant *little_endian_request_body =
    swap_bytes_if_big_endian (request_body);
  g_variant_unref (request_body);

  return little_endian_request_body;
}

static GVariant *
get_nullable_payload (GVariant *payload,
                      gboolean  has_payload)
{
  if (!has_payload)
    {
      g_variant_ref_sink (payload);
      g_variant_unref (payload);
      return NULL;
    }

  return payload;
}

static void
handle_network_monitor_can_reach (GNetworkMonitor *network_monitor,
                                  GAsyncResult    *result,
                                  EmerDaemon      *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (priv->uploading)
    return;

  GTask *upload_task = g_queue_pop_head (priv->upload_queue);

  GError *error = NULL;
  if (!g_network_monitor_can_reach_finish (network_monitor, result, &error))
    {
      flush_to_persistent_cache (self);
      g_task_return_error (upload_task, error);
      goto handle_upload_failed;
    }

  guint64 token;
  gsize num_buffer_events;
  GVariant *request_body =
    create_request_body (self, &token, &num_buffer_events, &error);
  if (request_body == NULL)
    {
      g_task_return_error (upload_task, error);
      goto handle_upload_failed;
    }

  gconstpointer serialized_request_body = g_variant_get_data (request_body);
  if (serialized_request_body == NULL)
    {
      g_task_return_new_error (upload_task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               "Could not serialize network request body.");
      goto handle_upload_failed;
    }

  priv->uploading = TRUE;

  gsize request_body_length = g_variant_get_size (request_body);

  SoupURI *http_request_uri =
    get_http_request_uri (self, serialized_request_body, request_body_length);
  SoupMessage *http_message =
    soup_message_new_from_uri ("PUT", http_request_uri);
  soup_uri_free (http_request_uri);

  soup_message_set_request (http_message, "application/octet-stream",
                            SOUP_MEMORY_TEMPORARY, serialized_request_body,
                            request_body_length);

  NetworkCallbackData *callback_data = g_new (NetworkCallbackData, 1);
  callback_data->daemon = self;
  callback_data->request_body = request_body;
  callback_data->token = token;
  callback_data->num_buffer_events = num_buffer_events;
  callback_data->attempt_num = 0;
  callback_data->upload_task = upload_task;
  soup_session_queue_message (priv->http_session, http_message,
                              (SoupSessionCallback) handle_http_response,
                              callback_data);
  return;

handle_upload_failed:
  g_object_unref (upload_task);
  g_signal_emit (self, emer_daemon_signals[SIGNAL_UPLOAD_FINISHED], 0u);
}

static GSocketConnectable *
get_ping_socket (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GError *error = NULL;
  GSocketConnectable *ping_socket =
    g_network_address_parse_uri (priv->server_uri, 443 /* SSL default port */,
                                 &error);
  if (ping_socket == NULL)
  {
    g_error ("Invalid server URI '%s' could not be parsed because: %s.",
             priv->server_uri, error->message);
  }

  return ping_socket;
}

static void
schedule_upload (EmerDaemon  *self,
                 const gchar *environment)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  guint network_send_interval;
  if (priv->network_send_interval != 0u)
    network_send_interval = priv->network_send_interval;
  else if (g_strcmp0 (environment, "production") == 0)
    network_send_interval = PRODUCTION_NETWORK_SEND_INTERVAL;
  else
    network_send_interval = DEV_NETWORK_SEND_INTERVAL;

  priv->upload_events_timeout_source_id =
    g_timeout_add_seconds (network_send_interval,
                           (GSourceFunc) handle_upload_timer,
                           self);
}

static gboolean
upload_permitted (EmerDaemon *self,
                  GError    **error)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (!priv->recording_enabled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   METRICS_DISABLED_MESSAGE);
      return FALSE;
    }

  gboolean uploading_enabled =
    emer_permissions_provider_get_uploading_enabled (priv->permissions_provider);
  if (!uploading_enabled)
    {
      flush_to_persistent_cache (self);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   UPLOADING_DISABLED_MESSAGE);
      return FALSE;
    }

  return TRUE;
}

static void
dequeue_and_do_upload (EmerDaemon  *self,
                       const gchar *environment)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GError *error = NULL;
  if (!upload_permitted (self, &error))
    {
      GTask *upload_task = g_queue_pop_head (priv->upload_queue);
      g_task_return_error (upload_task, error);
      g_object_unref (upload_task);
      g_signal_emit (self, emer_daemon_signals[SIGNAL_UPLOAD_FINISHED], 0u);
      return;
    }

  if (priv->use_default_server_uri)
    {
      g_free (priv->server_uri);
      priv->server_uri =
        g_strconcat ("https://", environment, ".metrics.endlessm.com/"
                     CLIENT_VERSION_NUMBER "/", NULL);
    }

  GNetworkMonitor *network_monitor = g_network_monitor_get_default ();
  GSocketConnectable *ping_socket = get_ping_socket (self);
  g_network_monitor_can_reach_async (network_monitor, ping_socket, NULL,
                                     (GAsyncReadyCallback) handle_network_monitor_can_reach,
                                     self);
  g_object_unref (ping_socket);
}

static void
upload_events (EmerDaemon         *self,
               const gchar        *environment,
               GAsyncReadyCallback callback,
               gpointer            user_data)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GTask *upload_task =
    g_task_new (self, NULL /* GCancellable */, callback, user_data);
  g_queue_push_tail (priv->upload_queue, upload_task);
  dequeue_and_do_upload (self, environment);
}

static void
log_upload_error (EmerDaemon   *self,
                  GAsyncResult *result,
                  gpointer      unused)
{
  GError *error = NULL;
  if (!emer_daemon_upload_events_finish (self, result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
          (g_strcmp0 (error->message, METRICS_DISABLED_MESSAGE) != 0 &&
           g_strcmp0 (error->message, UPLOADING_DISABLED_MESSAGE) != 0))
        g_warning ("Dropped events because they could not be uploaded: %s.",
                   error->message);
      g_error_free (error);
    }
}

static gboolean
handle_upload_timer (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  gchar *environment =
    emer_permissions_provider_get_environment (priv->permissions_provider);
  schedule_upload (self, environment);
  upload_events (self, environment, (GAsyncReadyCallback) log_upload_error,
                 NULL /* user_data */);
  g_free (environment);

  return G_SOURCE_REMOVE;
}

static void
handle_upload_finished (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (g_queue_is_empty (priv->upload_queue))
    return;

  gchar *environment =
    emer_permissions_provider_get_environment (priv->permissions_provider);
  dequeue_and_do_upload (self, environment);
  g_free (environment);
}

static void
inhibit_shutdown (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (priv->shutdown_inhibitor != -1)
    // There is no point in inhibiting shutdown twice.
    return;

  GVariant *inhibit_args = g_variant_new_parsed (INHIBIT_ARGS);
  GError *error = NULL;
  GUnixFDList *fd_list = NULL;
  GVariant *inhibitor_tuple =
    g_dbus_proxy_call_with_unix_fd_list_sync (priv->login_manager_proxy,
                                              "Inhibit", inhibit_args,
                                              G_DBUS_CALL_FLAGS_NONE,
                                              -1 /* timeout */,
                                              NULL /* input fd_list */,
                                              &fd_list, NULL /* GCancellable */,
                                              &error);

  if (inhibitor_tuple == NULL)
    {
      if (fd_list != NULL)
        g_object_unref (fd_list);
      g_warning ("Error inhibiting shutdown: %s.", error->message);
      g_error_free (error);
      return;
    }

  g_variant_unref (inhibitor_tuple);

  gint fd_list_length;
  gint *fds = g_unix_fd_list_steal_fds (fd_list, &fd_list_length);
  g_object_unref (fd_list);
  if (fd_list_length != 1)
    {
      g_warning ("Error inhibiting shutdown. Login manager returned %d file "
                 "descriptors, but we expected 1 file descriptor.",
                 fd_list_length);
      goto finally;
    }

  priv->shutdown_inhibitor = fds[0];

finally:
  g_free (fds);
}

static void
update_timestamps (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GError *error = NULL;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   NULL, TRUE, &error))
    {
      g_warning ("Persistent cache could not update timestamps: %s.",
                 error->message);
      g_error_free (error);
    }
}

static void
handle_login_manager_signal (GDBusProxy *dbus_proxy,
                             gchar      *sender_name,
                             gchar      *signal_name,
                             GVariant   *parameters,
                             EmerDaemon *self)
{
  if (g_strcmp0 ("PrepareForShutdown", signal_name) == 0)
    {
      gboolean shutting_down;
      g_variant_get_child (parameters, 0, "b", &shutting_down);
      if (shutting_down)
        {
          flush_to_persistent_cache (self);
          update_timestamps (self);
          release_shutdown_inhibitor (self);
        }
      else
        {
          inhibit_shutdown (self);
        }
    }
}

static void
register_with_login_manager (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  g_signal_connect (priv->login_manager_proxy, "g-signal",
                    G_CALLBACK (handle_login_manager_signal), self);
  inhibit_shutdown (self);
}

static gboolean
has_owner (GDBusProxy *dbus_proxy)
{
  gchar *name_owner =
    g_dbus_proxy_get_name_owner (dbus_proxy);
  if (name_owner != NULL)
    g_free (name_owner);

  return name_owner != NULL;
}

static void
handle_login_manager_name_owner_set (GDBusProxy *dbus_proxy,
                                     GParamSpec *pspec,
                                     EmerDaemon *self)
{
  if (has_owner (dbus_proxy))
    register_with_login_manager (self);
}

static void
connect_to_login_manager (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GError *error = NULL;
  priv->login_manager_proxy =
    g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                   G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                   NULL /* GDBusInterfaceInfo */,
                                   "org.freedesktop.login1",
                                   "/org/freedesktop/login1",
                                   "org.freedesktop.login1.Manager",
                                   NULL /* GCancellable */,
                                   &error);
  if (priv->login_manager_proxy == NULL)
    {
      g_warning ("Error creating login manager D-Bus proxy: %s.",
                 error->message);
      g_error_free (error);
      return;
    }

  if (has_owner (priv->login_manager_proxy))
    register_with_login_manager (self);
  else
    g_signal_connect (priv->login_manager_proxy, "notify::g-name-owner",
                      G_CALLBACK (handle_login_manager_name_owner_set), self);
}

static gchar *
get_user_agent (void)
{
  guint libsoup_major_version = soup_get_major_version ();
  guint libsoup_minor_version = soup_get_minor_version ();
  guint libsoup_micro_version = soup_get_micro_version ();
  return g_strdup_printf ("libsoup/%u.%u.%u", libsoup_major_version,
                          libsoup_minor_version, libsoup_micro_version);
}

static void
on_permissions_changed (EmerPermissionsProvider *permissions_provider,
                        GParamSpec              *pspec,
                        EmerDaemon              *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->recording_enabled =
    emer_permissions_provider_get_daemon_enabled (permissions_provider);
}

/*
 * The following functions are private setters for the properties of EmerDaemon.
 * These properties are write-only, construct-only, so these only need to be
 * internal.
 */

static void
set_random_number_generator (EmerDaemon *self,
                             GRand      *rand)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->rand = rand == NULL ? g_rand_new () : rand;
}

static void
set_server_uri (EmerDaemon  *self,
                const gchar *server_uri)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  priv->use_default_server_uri = (server_uri == NULL);
  if (!priv->use_default_server_uri)
    {
      g_free (priv->server_uri);
      priv->server_uri =
        g_build_filename (server_uri, CLIENT_VERSION_NUMBER "/", NULL);
    }
}

static void
set_network_send_interval (EmerDaemon *self,
                           guint       seconds)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->network_send_interval = seconds;
}

static void
set_machine_id_provider (EmerDaemon            *self,
                         EmerMachineIdProvider *machine_id_prov)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (machine_id_prov == NULL)
    priv->machine_id_provider = emer_machine_id_provider_new ();
  else
    priv->machine_id_provider = g_object_ref (machine_id_prov);
}

static void
set_network_send_provider (EmerDaemon *self,
                           EmerNetworkSendProvider *network_send_prov)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (network_send_prov == NULL)
    priv->network_send_provider = emer_network_send_provider_new ();
  else
    priv->network_send_provider = g_object_ref (network_send_prov);
}

static void
set_permissions_provider (EmerDaemon              *self,
                          EmerPermissionsProvider *permissions_provider)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (permissions_provider == NULL)
    priv->permissions_provider = emer_permissions_provider_new ();
  else
    priv->permissions_provider = g_object_ref (permissions_provider);

  g_signal_connect (priv->permissions_provider, "notify::daemon-enabled",
                    G_CALLBACK (on_permissions_changed), self);
  priv->recording_enabled =
    emer_permissions_provider_get_daemon_enabled (priv->permissions_provider);
}

static void
set_persistent_cache (EmerDaemon          *self,
                      EmerPersistentCache *persistent_cache)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (persistent_cache == NULL)
    {
      GError *error = NULL;
      priv->persistent_cache =
        emer_persistent_cache_new (PERSISTENT_CACHE_DIR, &error);
      if (priv->persistent_cache == NULL)
        {
          g_warning ("Could not create persistent cache: %s.", error->message);
          g_error_free (error);
        }
    }
  else
    {
      priv->persistent_cache = g_object_ref (persistent_cache);
    }
}

static void
set_max_bytes_buffered (EmerDaemon *self,
                        gsize       max_bytes_buffered)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->max_bytes_buffered = max_bytes_buffered;
}

static void
emer_daemon_constructed (GObject *object)
{
  EmerDaemon *self = EMER_DAEMON (object);
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  gchar *environment =
    emer_permissions_provider_get_environment (priv->permissions_provider);
  schedule_upload (self, environment);
  g_free (environment);

  G_OBJECT_CLASS (emer_daemon_parent_class)->constructed (object);
}

static void
emer_daemon_set_property (GObject      *object,
                          guint         property_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  EmerDaemon *self = EMER_DAEMON (object);

  switch (property_id)
    {
    case PROP_RANDOM_NUMBER_GENERATOR:
      set_random_number_generator (self, g_value_get_pointer (value));
      break;

    case PROP_SERVER_URI:
      set_server_uri (self, g_value_get_string (value));
      break;

    case PROP_NETWORK_SEND_INTERVAL:
      set_network_send_interval (self, g_value_get_uint (value));
      break;

    case PROP_MACHINE_ID_PROVIDER:
      set_machine_id_provider (self, g_value_get_object (value));
      break;

    case PROP_NETWORK_SEND_PROVIDER:
      set_network_send_provider (self, g_value_get_object (value));
      break;

    case PROP_PERMISSIONS_PROVIDER:
      set_permissions_provider (self, g_value_get_object (value));
      break;

    case PROP_PERSISTENT_CACHE:
      set_persistent_cache (self, g_value_get_object (value));
      break;

    case PROP_MAX_BYTES_BUFFERED:
      set_max_bytes_buffered (self, g_value_get_ulong (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_daemon_finalize (GObject *object)
{
  EmerDaemon *self = EMER_DAEMON (object);
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  g_source_remove (priv->upload_events_timeout_source_id);

  flush_to_persistent_cache (self);
  g_clear_object (&priv->persistent_cache);
  release_shutdown_inhibitor (self);

  g_clear_object (&priv->login_manager_proxy);

  g_queue_free_full (priv->upload_queue, g_object_unref);

  soup_session_abort (priv->http_session);
  g_clear_object (&priv->http_session);

  g_clear_pointer (&priv->variant_array, g_ptr_array_unref);

  g_rand_free (priv->rand);
  g_free (priv->server_uri);
  g_clear_object (&priv->machine_id_provider);
  g_clear_object (&priv->network_send_provider);
  g_clear_object (&priv->permissions_provider);

  G_OBJECT_CLASS (emer_daemon_parent_class)->finalize (object);
}

static void
emer_daemon_class_init (EmerDaemonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = emer_daemon_constructed;
  object_class->set_property = emer_daemon_set_property;
  object_class->finalize = emer_daemon_finalize;

  /*
   * EmerDaemon:random-number-generator:
   *
   * The random number generator is used to generate a random factor that scales
   * the delay between network retries.
   * In the case where a network request failed because a server was overloaded,
   * randomized backoff decreases the chances that the same set of clients will
   * overwhelm the same server when they retry.
   */
  emer_daemon_props[PROP_RANDOM_NUMBER_GENERATOR] =
    g_param_spec_pointer ("random-number-generator", "Random number generator",
                          "Random number generator used to randomize "
                          "exponential backoff",
                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                          G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:server-uri:
   *
   * The URI to which events are uploaded. The URI must contain the protocol and
   * may contain the port number. If unspecified, the port number defaults to
   * 443, which is the standard port number for SSL.
   */
  emer_daemon_props[PROP_SERVER_URI] =
    g_param_spec_string ("server-uri", "Server URI",
                         "URI to which events are uploaded",
                         NULL,
                         G_PARAM_CONSTRUCT | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:network-send-interval:
   *
   * The frequency with which the client will attempt a network send request, in
   * seconds.
   */
  emer_daemon_props[PROP_NETWORK_SEND_INTERVAL] =
    g_param_spec_uint ("network-send-interval", "Network send interval",
                       "Number of seconds between attempts to flush metrics to "
                       "proxy server",
                       0, G_MAXUINT, 0,
                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                       G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:machine-id-provider:
   *
   * An #EmerMachineIdProvider for retrieving the UUID of this machine.
   * If this property is not specified, the default machine ID provider (from
   * emer_machine_id_provider_new()) will be used.
   */
  emer_daemon_props[PROP_MACHINE_ID_PROVIDER] =
    g_param_spec_object ("machine-id-provider", "Machine ID provider",
                         "Object providing machine ID",
                         EMER_TYPE_MACHINE_ID_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:network-send-provider:
   *
   * An #EmerNetworkSendProvider for getting and setting the network send
   * metadata. If this property is not specified, the default network send
   * provider will be used (from emer_network_send_provider_new()).
   */
  emer_daemon_props[PROP_NETWORK_SEND_PROVIDER] =
    g_param_spec_object ("network-send-provider", "Network send provider",
                         "Object providing network send metadata",
                         EMER_TYPE_NETWORK_SEND_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:permissions-provider:
   *
   * An #EmerPermissionsProvider for getting the user's preferences regarding
   * the metrics system. If this property is not specified, the default
   * permissions provider will be used (from emer_permissions_provider_new()).
   */
  emer_daemon_props[PROP_PERMISSIONS_PROVIDER] =
    g_param_spec_object ("permissions-provider", "Permissions provider",
                         "Object providing user's permission to record metrics",
                         EMER_TYPE_PERMISSIONS_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:persistent-cache:
   *
   * An #EmerPersistentCache for storing metrics until they are sent to the
   * proxy server.
   * If this property is not specified, a default persistent cache (created by
   * emer_persistent_cache_new ()) will be used.
   */
  emer_daemon_props[PROP_PERSISTENT_CACHE] =
    g_param_spec_object ("persistent-cache", "Persistent cache",
                         "Object managing persistent storage of metrics until "
                         "they are sent to the proxy server",
                         EMER_TYPE_PERSISTENT_CACHE,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_MAX_BYTES_BUFFERED] =
    g_param_spec_ulong ("max-bytes-buffered", "Max bytes buffered",
                        "The maximum number of bytes of event data that may be "
                        "buffered in memory. Does not include overhead.",
                        0ul, G_MAXULONG, 100000ul /* 100 kB */,
                        G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                        G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS, emer_daemon_props);

  klass->upload_finished_handler = handle_upload_finished;

  emer_daemon_signals[SIGNAL_UPLOAD_FINISHED] =
    g_signal_new ("upload-finished", EMER_TYPE_DAEMON, G_SIGNAL_RUN_FIRST,
                  G_STRUCT_OFFSET (EmerDaemonClass, upload_finished_handler),
                  NULL /* GSignalAccumulator */, NULL /* accumulator_data */,
                  g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0u);
}

static void
emer_daemon_init (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  // We are not currently holding a shutdown inhibitor.
  priv->shutdown_inhibitor = -1;

  connect_to_login_manager (self);

  priv->upload_queue = g_queue_new ();

  gchar *user_agent = get_user_agent ();

  priv->http_session =
    soup_session_new_with_options (SOUP_SESSION_MAX_CONNS, 1,
                                   SOUP_SESSION_MAX_CONNS_PER_HOST, 1,
                                   SOUP_SESSION_USER_AGENT, user_agent,
                                   SOUP_SESSION_ADD_FEATURE_BY_TYPE,
                                   SOUP_TYPE_CACHE,
                                   NULL);

  g_free (user_agent);

  priv->variant_array =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);
}

/*
 * emer_daemon_new:
 *
 * Returns: (transfer full): a new #EmerDaemon with the default configuration.
 */
EmerDaemon *
emer_daemon_new (void)
{
  return g_object_new (EMER_TYPE_DAEMON, NULL);
}

/*
 * emer_daemon_new_full:
 * @rand: (allow-none): random number generator to use for randomized
 *   exponential backoff, or %NULL to use the default.
 * @server_uri: (allow-none): the URI (including protocol and, optionally, port
 *   number) to which to upload events, or %NULL to use the default. Must
 *   include trailing forward slash. If the port number is unspecified, it
 *   defaults to 443 (the standard port used by SSL).
 * @network_send_interval: frequency in seconds with which the client will
 *   attempt a network send request.
 * @machine_id_provider: (allow-none): The #EmerMachineIdProvider to supply the
 *   machine ID, or %NULL to use the default.
 * @network_send_provider: (allow-none): The #EmerNetworkSendProvider to supply
 *   the network send metadata, or %NULL to use the default.
 * @permissions_provider: The #EmerPermissionsProvider to supply information
 *   about opting out of metrics collection, disabling network uploads, and the
 *   metrics environment (dev or production).
 * @persistent_cache: (allow-none): The #EmerPersistentCache in which to store
 *   metrics locally when they can't be sent over the network, or %NULL to use
 *   the default.
 * @max_bytes_buffered: The maximum number of bytes of event data that may be
 *   stored in memory. Does not include overhead.
 *
 * Returns: (transfer full): a new customized #EmerDaemon. Use emer_daemon_new
 * to use the default configuration.
 */
EmerDaemon *
emer_daemon_new_full (GRand                   *rand,
                      const gchar             *server_uri,
                      guint                    network_send_interval,
                      EmerMachineIdProvider   *machine_id_provider,
                      EmerNetworkSendProvider *network_send_provider,
                      EmerPermissionsProvider *permissions_provider,
                      EmerPersistentCache     *persistent_cache,
                      gulong                   max_bytes_buffered)
{
  return g_object_new (EMER_TYPE_DAEMON,
                       "random-number-generator", rand,
                       "server-uri", server_uri,
                       "network-send-interval", network_send_interval,
                       "machine-id-provider", machine_id_provider,
                       "network-send-provider", network_send_provider,
                       "permissions-provider", permissions_provider,
                       "persistent-cache", persistent_cache,
                       "max-bytes-buffered", max_bytes_buffered,
                       NULL);
}

void
emer_daemon_record_singular_event (EmerDaemon *self,
                                   guint32     user_id,
                                   GVariant   *event_id,
                                   gint64      relative_timestamp,
                                   gboolean    has_payload,
                                   GVariant   *payload)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (!priv->recording_enabled)
    return;

  if (!is_uuid (event_id))
    {
      g_warning ("Event ID must be a UUID represented as an array of %d "
                 "bytes. Dropping event.", UUID_LENGTH);
      return;
    }

  gint64 boot_offset;
  GError *error = NULL;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, FALSE, &error))
    {
      g_warning ("Unable to correct event's relative timestamp. Dropping "
                 "event. Error: %s.", error->message);
      g_error_free (error);
      return;
    }
  relative_timestamp += boot_offset;

  GVariant *nullable_payload = get_nullable_payload (payload, has_payload);
  GVariant *singular =
    g_variant_new ("(u@ayxmv)", user_id, event_id, relative_timestamp,
                   nullable_payload);
  buffer_event (self, singular);
}

void
emer_daemon_record_aggregate_event (EmerDaemon *self,
                                    guint32     user_id,
                                    GVariant   *event_id,
                                    gint64      num_events,
                                    gint64      relative_timestamp,
                                    gboolean    has_payload,
                                    GVariant   *payload)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (!priv->recording_enabled)
    return;

  if (!is_uuid (event_id))
    {
      g_warning ("Event ID must be a UUID represented as an array of %d "
                 "bytes. Dropping event.", UUID_LENGTH);
      return;
    }

  gint64 boot_offset;
  GError *error = NULL;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, FALSE, &error))
    {
      g_warning ("Unable to correct event's relative timestamp. Dropping "
                 "event. Error: %s.", error->message);
      g_error_free (error);
      return;
    }
  relative_timestamp += boot_offset;

  GVariant *nullable_payload = get_nullable_payload (payload, has_payload);
  GVariant *aggregate =
    g_variant_new ("(u@ayxxmv)", user_id, event_id, num_events,
                   relative_timestamp, nullable_payload);
  buffer_event (self, aggregate);
}

void
emer_daemon_record_event_sequence (EmerDaemon *self,
                                   guint32     user_id,
                                   GVariant   *event_id,
                                   GVariant   *event_values)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (!priv->recording_enabled)
    return;

  if (!is_uuid (event_id))
    {
      g_warning ("Event ID must be a UUID represented as an array of %d "
                 "bytes. Dropping event.", UUID_LENGTH);
      return;
    }

  gint64 boot_offset;
  GError *error = NULL;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, FALSE, &error))
    {
      g_warning ("Unable to correct event's relative timestamp. Dropping "
                 "event. Error: %s.", error->message);
      g_error_free (error);
      return;
    }

  g_variant_ref_sink (event_values);
  gsize num_event_values = g_variant_n_children (event_values);
  GVariantBuilder event_values_builder;
  g_variant_builder_init (&event_values_builder, EVENT_VALUE_ARRAY_TYPE);
  for (gsize i = 0; i < num_event_values; i++)
    {
      gint64 relative_timestamp;
      gboolean has_payload;
      GVariant *payload;
      g_variant_get_child (event_values, i, "(xbv)", &relative_timestamp,
                           &has_payload, &payload);

      relative_timestamp += boot_offset;
      GVariant *nullable_payload = get_nullable_payload (payload, has_payload);
      g_variant_builder_add (&event_values_builder, EVENT_VALUE_TYPE_STRING,
                             relative_timestamp, nullable_payload);
    }

  g_variant_unref (event_values);
  GVariant *sequence =
    g_variant_new ("(u@ay" EVENT_VALUE_ARRAY_TYPE_STRING ")", user_id, event_id,
                   &event_values_builder);
  buffer_event (self, sequence);
}

/* emer_daemon_upload_events:
 * @self: the daemon
 * @callback (nullable): the function to call once the upload completes. The
 * first parameter passed to this callback is self. The second parameter is a
 * GAsyncResult that can be passed to emer_daemon_upload_events_finish to
 * determine whether the upload succeeded. The third parameter is user_data.
 * @user_data (nullable): arbitrary data that is blindly passed through to the
 * callback.
 *
 * The event recorder daemon may have already decided to upload some or all
 * events before this method was called. Once events have been uploaded, they
 * may no longer be stored locally.
 */
void
emer_daemon_upload_events (EmerDaemon         *self,
                           GAsyncReadyCallback callback,
                           gpointer            user_data)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  gchar *environment =
    emer_permissions_provider_get_environment (priv->permissions_provider);
  upload_events (self, environment, callback, user_data);
  g_free (environment);
}

/* emer_daemon_upload_events_finish:
 * @self: the daemon
 * @result: a GAsyncResult that encapsulates whether the upload succeeded
 * @error (out) (optional): if the upload failed, error will be set to a GError
 * describing what went wrong; otherwise it will be set to NULL. Pass NULL to
 * ignore this value.
 *
 * Returns: TRUE if the upload succeeded (even if there were no events to
 * upload) and FALSE if it failed.
 */
gboolean
emer_daemon_upload_events_finish (EmerDaemon   *self,
                                  GAsyncResult *result,
                                  GError      **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

/*
 * emer_daemon_get_permissions_provider:
 * @self: the daemon
 *
 * This is a public property accessor so that the DBus calls can communicate
 * directly with the permissions provider.
 *
 * Returns: (transfer none): the daemon's permissions provider
 */
EmerPermissionsProvider *
emer_daemon_get_permissions_provider (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->permissions_provider;
}
