/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014-2017 Endless Mobile, Inc. */

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

#include "config.h"
#include "emer-daemon.h"

#include <math.h>
#include <time.h>
#include <uuid/uuid.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include <eosmetrics/eosmetrics.h>

#include "eins-boottime-source.h"
#include "emer-aggregate-tally.h"
#include "emer-aggregate-timer-impl.h"
#include "emer-gzip.h"
#include "emer-image-id-provider.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"
#include "emer-site-id-provider.h"
#include "emer-types.h"
#include "shared/metrics-util.h"

/*
 * The version of this client's network protocol.
 */
#define CLIENT_VERSION_NUMBER "3"

/*
 * The minimum number of seconds to wait before attempting the first retry of a
 * network request that failed with a non-fatal error.
 */
#define INITIAL_BACKOFF_SEC 1

/*
 * The maximum number of attempts to upload a particular batch of metric events
 * before giving up.
 */
#define NETWORK_ATTEMPT_LIMIT 8

/*
 * How many seconds to delay between trying to send events to the metrics
 * servers if we are online, or to the persistent cache, if we are offline.
 *
 * For QA, the "dev" environment delay is much shorter.
 */
#define DEV_NETWORK_SEND_INTERVAL (60u * 15u) // Fifteen minutes
#define PRODUCTION_NETWORK_SEND_INTERVAL (60u * 30u) // Thirty minutes

#define EVENT_VALUE_TYPE_STRING "(xmv)"
#define EVENT_VALUE_ARRAY_TYPE_STRING "a" EVENT_VALUE_TYPE_STRING
#define EVENT_VALUE_ARRAY_TYPE G_VARIANT_TYPE (EVENT_VALUE_ARRAY_TYPE_STRING)

#define SINGULAR_TYPE_STRING "(aysxmv)"
#define AGGREGATE_TYPE_STRING "(ayssumv)"

#define SINGULAR_TYPE G_VARIANT_TYPE (SINGULAR_TYPE_STRING)
#define AGGREGATE_TYPE G_VARIANT_TYPE (AGGREGATE_TYPE_STRING)

#define SINGULAR_ARRAY_TYPE_STRING "a" SINGULAR_TYPE_STRING
#define AGGREGATE_ARRAY_TYPE_STRING "a" AGGREGATE_TYPE_STRING

#define SINGULAR_ARRAY_TYPE G_VARIANT_TYPE (SINGULAR_ARRAY_TYPE_STRING)
#define AGGREGATE_ARRAY_TYPE G_VARIANT_TYPE (AGGREGATE_ARRAY_TYPE_STRING)

#define REQUEST_TYPE_STRING "(xxs@a{ss}y" SINGULAR_ARRAY_TYPE_STRING \
  AGGREGATE_ARRAY_TYPE_STRING ")"

#define RETRY_TYPE_STRING "(xxs@a{ss}y@" SINGULAR_ARRAY_TYPE_STRING "@" \
  AGGREGATE_ARRAY_TYPE_STRING ")"

/* This limit only applies to timer-driven uploads, not explicitly
 * requested uploads.
 */
#define MAX_REQUEST_PAYLOAD 100000 /* 100 kB */

#define METRICS_DISABLED_MESSAGE "Could not upload events because the " \
  "metrics system is disabled. You may enable the metrics system via " \
  "Settings > Privacy > Metrics"

#define UPLOADING_DISABLED_MESSAGE "Could not upload events because " \
  "uploading is disabled. You may enable uploading by setting " \
  "uploading_enabled to true in " PERMISSIONS_FILE


typedef struct _NetworkCallbackData
{
  GVariant *request_body;
  guint64 token;
  gsize max_upload_size;
  gsize num_stored_events;
  gsize num_buffer_events;
  gint attempt_num;
  guint backoff_timeout_source_id;
} NetworkCallbackData;

typedef struct _AggregateTimerSenderData
{
  guint watch_id;
  GPtrArray *aggregate_timers;
} AggregateTimerSenderData;

struct _EmerDaemon
{
  GObject parent;

  guint network_send_interval;
  GQueue *upload_queue;

  /* Non-NULL iff an upload is in flight. An owned reference to the
   * cancellable held by NetworkCallbackData.upload_task.
   */
  GCancellable *current_upload_cancellable;

  SoupSession *http_session;

  GPtrArray *variant_array;
  gsize num_bytes_buffered;
  gboolean have_logged_overflow;

  GHashTable *aggregate_timers;
  GHashTable *monitored_senders;

  /* Private storage for public properties */

  GRand *rand;

  gchar *server_url;

  guint upload_events_timeout_source_id;
  guint report_invalid_cache_data_source_id;
  guint dispatch_aggregate_timers_daily_source_id;

  GDateTime *current_aggregate_tally_date;

  EmerAggregateTally *aggregate_tally;
  EmerPermissionsProvider *permissions_provider;

  gchar *persistent_cache_directory;
  EmerPersistentCache *persistent_cache;

  gboolean recording_enabled;

  gsize max_bytes_buffered;
};

G_DEFINE_TYPE (EmerDaemon, emer_daemon, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_RANDOM_NUMBER_GENERATOR,
  PROP_SERVER_URL,
  PROP_NETWORK_SEND_INTERVAL,
  PROP_PERMISSIONS_PROVIDER,
  PROP_PERSISTENT_CACHE_DIRECTORY,
  PROP_PERSISTENT_CACHE,
  PROP_AGGREGATE_TALLY,
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

static void handle_http_response (GObject      *source_object,
                                  GAsyncResult *result,
                                  GTask        *upload_task);

static void
aggregate_timer_sender_data_free (AggregateTimerSenderData *sender_data)
{
  if (!sender_data)
    return;

  g_clear_handle_id (&sender_data->watch_id, g_bus_unwatch_name);
  g_clear_pointer (&sender_data->aggregate_timers, g_ptr_array_unref);
  g_free (sender_data);
}

static void
network_callback_data_free (NetworkCallbackData *callback_data)
{
  if (callback_data->backoff_timeout_source_id != 0)
    g_source_remove (callback_data->backoff_timeout_source_id);
  g_clear_pointer (&callback_data->request_body, g_variant_unref);
  g_free (callback_data);
}

static void
finish_network_callback (GTask *upload_task)
{
  EmerDaemon *self = g_task_get_source_object (upload_task);

  g_clear_object (&self->current_upload_cancellable);

  g_signal_emit (self, emer_daemon_signals[SIGNAL_UPLOAD_FINISHED], 0u);
  g_object_unref (upload_task);
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
  g_variant_ref_sink (event);
  gsize event_cost = emer_persistent_cache_cost (event);
  gsize new_bytes_buffered = self->num_bytes_buffered + event_cost;
  if (event_cost > MAX_REQUEST_PAYLOAD || /* Don't get wedged by large event. */
      new_bytes_buffered > self->max_bytes_buffered)
    {
      g_variant_unref (event);
      if (event_cost > MAX_REQUEST_PAYLOAD)
        {
          g_warning ("Dropping %" G_GSIZE_FORMAT "-byte event. The maximum "
                     "permissible event size (including type string with "
                     "null-terminating byte) is %d bytes.",
                     event_cost, MAX_REQUEST_PAYLOAD);
        }
      else if (!self->have_logged_overflow)
        {
          g_warning ("The event buffer overflowed for the first time in the "
                     "life of this event recorder daemon. The maximum number "
                     "of bytes that may be buffered is %" G_GSIZE_FORMAT ".",
                     self->max_bytes_buffered);
          self->have_logged_overflow = TRUE;
        }
      return;
    }

  g_ptr_array_add (self->variant_array, event);
  self->num_bytes_buffered = new_bytes_buffered;
}

static void
remove_events (EmerDaemon *self,
               gsize       num_events)
{
  if (num_events == 0)
    return;

  for (gsize i = 0; i < num_events; i++)
    {
      GVariant *curr_variant = g_ptr_array_index (self->variant_array, i);
      self->num_bytes_buffered -= emer_persistent_cache_cost (curr_variant);
    }

  g_ptr_array_remove_range (self->variant_array, 0u, num_events);
}

static void
flush_to_persistent_cache (EmerDaemon *self)
{
  if (!self->recording_enabled)
    return;

  if (self->variant_array->len == 0)
    return;

  gsize num_events_stored;
  GError *error = NULL;
  gboolean store_succeeded =
    emer_persistent_cache_store (self->persistent_cache,
                                 (GVariant **) self->variant_array->pdata,
                                 self->variant_array->len,
                                 &num_events_stored,
                                 &error);
  if (!store_succeeded)
    {
      g_warning ("Failed to flush buffer to persistent cache: %s.",
                 error->message);
      g_error_free (error);
      return;
    }

  g_message ("Flushed %" G_GSIZE_FORMAT " events to persistent cache.",
             num_events_stored);
  remove_events (self, num_events_stored);
}

static void
remove_from_persistent_cache (EmerDaemon *self,
                              guint64     token)
{
  GError *error = NULL;
  gboolean remove_succeeded =
    emer_persistent_cache_remove (self->persistent_cache, token, &error);
  if (!remove_succeeded)
    {
      g_warning ("Failed to remove events from persistent cache with token %"
                 G_GUINT64_FORMAT ". They may be resent to the server. Error: "
                 "%s.", token, error->message);
      g_error_free (error);
    }
}

static guint
get_random_backoff_interval (GRand *rand,
                             gint   attempt_num)
{
  gulong base_backoff_sec = INITIAL_BACKOFF_SEC;
  for (gint i = 0; i < attempt_num - 1; i++)
    base_backoff_sec *= 2;

  gdouble random_factor = g_rand_double_range (rand, 1, 2);
  gdouble randomized_backoff_sec = random_factor * (gdouble) base_backoff_sec;

  return (guint) round (randomized_backoff_sec);
}

/*
 * Returned object is owned by calling code. Free with g_uri_unref() when
 * done.
 */
static GUri *
get_http_request_url (EmerDaemon   *self,
                      const guchar *data,
                      gsize         length)
{
  gchar *checksum =
    g_compute_checksum_for_data (G_CHECKSUM_SHA512, data, length);
  gchar *http_request_url_string =
    g_build_filename (self->server_url, checksum, NULL);
  g_free (checksum);

  g_autoptr(GError) error = NULL;
  GUri *http_request_url = g_uri_parse (http_request_url_string, SOUP_HTTP_URI_FLAGS, &error);

  if (http_request_url == NULL)
    g_error ("Invalid http request URL '%s' could not be parsed because: %s.",
             http_request_url_string, error->message);

  g_free (http_request_url_string);

  return http_request_url;
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
                   "Could not get current time");
      return FALSE;
    }


  gint64 boot_offset;
  if (!emer_persistent_cache_get_boot_time_offset (self->persistent_cache,
                                                   &boot_offset, error))
    return FALSE;

  *rel_timestamp_ptr += boot_offset;
  return TRUE;
}

static GVariant *
get_updated_request_body (EmerDaemon *self,
                          GVariant   *request_body,
                          GError    **error)
{
  g_autofree gchar *image_version;
  GVariant *site_id, *singulars, *aggregates;
  guint8 boot_type;
  g_variant_get (request_body, RETRY_TYPE_STRING,
                 NULL /* relative time */, NULL /* absolute time */,
                 &image_version, &site_id, &boot_type, &singulars, &aggregates);

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

  return g_variant_new (RETRY_TYPE_STRING,
                        little_endian_relative_timestamp,
                        little_endian_absolute_timestamp,
                        image_version, site_id, boot_type,
                        singulars, aggregates);
}

static void
queue_http_request (GTask *upload_task)
{
  EmerDaemon *self = g_task_get_source_object (upload_task);
  NetworkCallbackData *callback_data = g_task_get_task_data (upload_task);

  gconstpointer serialized_request_body =
    g_variant_get_data (callback_data->request_body);
  if (serialized_request_body == NULL)
    {
      g_task_return_new_error (upload_task, G_IO_ERROR,
                               G_IO_ERROR_INVALID_DATA,
                               "Could not serialize network request body");
      finish_network_callback (upload_task);
      return;
    }

  gsize serialized_request_body_length =
    g_variant_get_size (callback_data->request_body);

  gsize compressed_request_body_length;
  GError *error = NULL;
  gpointer compressed_request_body =
    emer_gzip_compress (serialized_request_body,
                        serialized_request_body_length,
                        &compressed_request_body_length,
                        &error);
  if (compressed_request_body == NULL)
    {
      g_task_return_error (upload_task, error);
      finish_network_callback (upload_task);
      return;
    }

  g_autoptr(GUri) http_request_url =
    get_http_request_url (self, serialized_request_body,
                          serialized_request_body_length);
  g_autoptr(SoupMessage) http_message =
    soup_message_new_from_uri ("PUT", http_request_url);

  soup_message_headers_append (soup_message_get_request_headers (http_message),
                               "X-Endless-Content-Encoding", "gzip");

  g_autoptr(GBytes) request_body = g_bytes_new (compressed_request_body, compressed_request_body_length);
  soup_message_set_request_body_from_bytes (http_message, "application/octet-stream", request_body);

  soup_session_send_async (self->http_session, http_message, G_PRIORITY_DEFAULT, NULL,
                           (GAsyncReadyCallback) handle_http_response,
                           upload_task);
}

static gboolean
handle_backoff_timer (GTask *upload_task)
{
  EmerDaemon *self = g_task_get_source_object (upload_task);

  NetworkCallbackData *callback_data = g_task_get_task_data (upload_task);
  callback_data->backoff_timeout_source_id = 0;

  GError *error = NULL;
  GVariant *updated_request_body =
    get_updated_request_body (self, callback_data->request_body, &error);

  if (updated_request_body == NULL)
    {
      g_task_return_error (upload_task, error);
      finish_network_callback (upload_task);
      return G_SOURCE_REMOVE;
    }

  g_variant_unref (callback_data->request_body);
  callback_data->request_body = updated_request_body;
  queue_http_request (upload_task);

  return G_SOURCE_REMOVE;
}

// Handles HTTP or HTTPS responses.
static void
handle_http_response (GObject      *source_object,
                      GAsyncResult *result,
                      GTask        *upload_task)
{
  EmerDaemon *self = g_task_get_source_object (upload_task);
  NetworkCallbackData *callback_data = g_task_get_task_data (upload_task);
  g_autoptr(GError) error = NULL;

  g_autoptr(GInputStream) stream = soup_session_send_finish (SOUP_SESSION (source_object), result, &error);

  SoupMessage *http_message = soup_session_get_async_result_message (SOUP_SESSION (source_object), result);
  guint status_code = soup_message_get_status (http_message);

  if (stream != NULL && SOUP_STATUS_IS_SUCCESSFUL (status_code))
    {
      GCancellable *cancellable =
        g_task_get_cancellable (upload_task);

      if (g_cancellable_is_cancelled (cancellable))
        {
          /* Daemon was disabled, but we've already sent the request
           * successfully. Disabling the daemon discards all cached and
           * buffered events, so don't try to do that here too. Just allow the
           * task to return success.
           */
          g_cancellable_reset (cancellable);
        }
      else
        {
          remove_from_persistent_cache (self, callback_data->token);
          remove_events (self, callback_data->num_buffer_events);
          flush_to_persistent_cache (self);
        }

      g_message ("Uploaded "
                 "%" G_GSIZE_FORMAT " events from persistent cache, "
                 "%" G_GSIZE_FORMAT " events from buffer to %s.",
                 callback_data->num_stored_events,
                 callback_data->num_buffer_events,
                 self->server_url);
      g_task_return_boolean (upload_task, TRUE);
      finish_network_callback (upload_task);
      return;
    }

  const gchar *reason;
  if (error != NULL)
    reason = error->message;
  else
    reason = soup_message_get_reason_phrase (http_message);
  g_warning ("Attempt to upload metrics failed: %s.", reason);

  if (g_task_return_error_if_cancelled (upload_task))
    {
      finish_network_callback (upload_task);
      return;
    }

  if (++callback_data->attempt_num >= NETWORK_ATTEMPT_LIMIT)
    {
      g_task_return_new_error (upload_task, G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "Maximum number of network attempts (%d) "
                               "reached", NETWORK_ATTEMPT_LIMIT);
      finish_network_callback (upload_task);
      return;
    }

  if (SOUP_STATUS_IS_CLIENT_ERROR (status_code) ||
      SOUP_STATUS_IS_SERVER_ERROR (status_code) ||
      g_error_matches (error, SOUP_SESSION_ERROR, SOUP_SESSION_ERROR_PARSING) ||
      g_error_matches (error, SOUP_SESSION_ERROR, SOUP_SESSION_ERROR_ENCODING))
    {
      guint random_backoff_interval =
        get_random_backoff_interval (self->rand, callback_data->attempt_num);
      callback_data->backoff_timeout_source_id =
        g_timeout_add_seconds (random_backoff_interval,
                               (GSourceFunc) handle_backoff_timer,
                               upload_task);

      /* Old message is unreffed automatically, because it is not requeued. */
      return;
    }

  g_task_return_new_error (upload_task, G_IO_ERROR,
                           G_IO_ERROR_FAILED, "Received HTTP status code: %u",
                           status_code);
  finish_network_callback (upload_task);
}

static void
add_events_to_builders (GVariant       **events,
                        gsize            num_events,
                        GVariantBuilder *singulars,
                        GVariantBuilder *aggregates)
{
  for (gsize i = 0; i < num_events; i++)
    {
      GVariant *curr_event = events[i];
      const GVariantType *event_type = g_variant_get_type (curr_event);
      if (g_variant_type_equal (event_type, SINGULAR_TYPE))
        g_variant_builder_add_value (singulars, curr_event);
      else if (g_variant_type_equal (event_type, AGGREGATE_TYPE))
        g_variant_builder_add_value (aggregates, curr_event);
      else
        g_error ("An event has an unexpected variant type.");
    }
}

/* Populates the given variant builders with at most max_bytes of data from the
 * persistent cache. Returns TRUE if the current network request should also
 * include data from the in-memory buffer and FALSE otherwise. Sets read_bytes
 * to the number of bytes of data that were read and token to a value that can
 * be passed to emer_persistent_cache_remove to remove the events that were
 * added to the variant builders from the persistent cache.
 */
static gboolean
add_stored_events_to_builders (EmerDaemon        *self,
                               gsize              max_bytes,
                               gsize             *read_variants,
                               gsize             *read_bytes,
                               guint64           *token,
                               GVariantBuilder   *singulars,
                               GVariantBuilder   *aggregates)
{
  GVariant **variants;
  gsize num_variants;
  GError *error = NULL;
  gboolean has_invalid = FALSE;
  gboolean read_succeeded =
    emer_persistent_cache_read (self->persistent_cache, &variants, max_bytes,
                                &num_variants, token, &has_invalid, &error);
  if (!read_succeeded)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA))
        {
          GError *local_error = NULL;
          if (!emer_persistent_cache_remove_all (self->persistent_cache, &local_error))
            {
              g_warning ("Error removing data from the persistent cache: %s", local_error->message);
              g_error_free (local_error);
            }

          g_warning ("Corrupt data read from the persistent cache. All cleared");
        }
      else
        {
          g_warning ("Could not read from persistent cache: %s.", error->message);
        }

      g_error_free (error);
      *read_variants = 0;
      *read_bytes = 0;
      *token = 0;
      return TRUE;
    }

  if (has_invalid)
    {
      g_warning ("Invalid data found in the persistent cache: "
                 "%" G_GSIZE_FORMAT " valid records read (%" G_GUINT64_FORMAT " bytes read)",
                 num_variants, *token);
    }

  add_events_to_builders (variants, num_variants,
                          singulars, aggregates);

  gsize curr_read_bytes = 0;
  for (gsize i = 0; i < num_variants; i++)
    curr_read_bytes += emer_persistent_cache_cost (variants[i]);

  g_free (variants);

  *read_variants = num_variants;
  *read_bytes = curr_read_bytes;

  return !emer_persistent_cache_has_more (self->persistent_cache, *token);
}

static void
add_buffered_events_to_builders (EmerDaemon      *self,
                                 gsize            num_bytes,
                                 gsize           *num_variants,
                                 GVariantBuilder *singulars,
                                 GVariantBuilder *aggregates)
{
  gsize curr_bytes = 0, curr_num_variants = 0;
  for (; curr_num_variants < self->variant_array->len; curr_num_variants++)
    {
      GVariant *curr_event =
        g_ptr_array_index (self->variant_array, curr_num_variants);
      curr_bytes += emer_persistent_cache_cost (curr_event);
      if (curr_bytes > num_bytes)
        break;
    }

  add_events_to_builders ((GVariant **) self->variant_array->pdata,
                          curr_num_variants, singulars, aggregates);
  *num_variants = curr_num_variants;
}

static GVariant *
create_request_body (EmerDaemon *self,
                     gsize       max_bytes,
                     guint64    *token,
                     gsize      *num_stored_events,
                     gsize      *num_buffer_events,
                     GError    **error)
{
  GVariantBuilder singulars, aggregates;
  g_variant_builder_init (&singulars, SINGULAR_ARRAY_TYPE);
  g_variant_builder_init (&aggregates, AGGREGATE_ARRAY_TYPE);

  g_autofree gchar *image_version = emer_image_id_provider_get_version ();
  GVariant *site_id = emer_site_id_provider_get_id ();
  guint8 boot_type = emer_boot_id_provider_get_boot_type ();

  gsize num_bytes_read;
  gboolean add_from_buffer =
    add_stored_events_to_builders (self, max_bytes, num_stored_events,
                                   &num_bytes_read, token,
                                   &singulars, &aggregates);

  if (add_from_buffer)
    {
      gsize space_remaining = max_bytes - num_bytes_read;
      add_buffered_events_to_builders (self, space_remaining, num_buffer_events,
                                       &singulars, &aggregates);
    }
  else
    {
      *num_buffer_events = 0;
    }

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_offset_timestamps (self, &relative_timestamp, &absolute_timestamp,
                              error))
    return NULL;

  GVariant *request_body =
    g_variant_new (REQUEST_TYPE_STRING, relative_timestamp, absolute_timestamp,
                   image_version, site_id, boot_type, &singulars, &aggregates);

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
  g_return_val_if_fail (payload != NULL, NULL);

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
  if (self->current_upload_cancellable != NULL)
    return;

  GTask *upload_task = g_queue_pop_head (self->upload_queue);

  GError *error = NULL;
  if (!g_network_monitor_can_reach_finish (network_monitor, result, &error))
    {
      flush_to_persistent_cache (self);
      g_task_return_error (upload_task, error);
      g_signal_emit (self, emer_daemon_signals[SIGNAL_UPLOAD_FINISHED], 0u);
      return;
    }

  NetworkCallbackData *callback_data = g_task_get_task_data (upload_task);
  guint64 token;
  gsize num_stored_events;
  gsize num_buffer_events;
  GVariant *request_body =
    create_request_body (self, callback_data->max_upload_size, &token,
                         &num_stored_events, &num_buffer_events, &error);
  if (request_body == NULL)
    {
      g_task_return_error (upload_task, error);
      g_signal_emit (self, emer_daemon_signals[SIGNAL_UPLOAD_FINISHED], 0u);
      return;
    }

  self->current_upload_cancellable = g_object_ref (
    g_task_get_cancellable (upload_task));

  callback_data->request_body = request_body;
  callback_data->token = token;
  callback_data->num_stored_events = num_stored_events;
  callback_data->num_buffer_events = num_buffer_events;
  callback_data->attempt_num = 0;

  queue_http_request (upload_task);
}

static GSocketConnectable *
get_ping_socket (EmerDaemon *self)
{
  GError *error = NULL;
  GSocketConnectable *ping_socket =
    g_network_address_parse_uri (self->server_url, 443 /* SSL default port */,
                                 &error);
  if (ping_socket == NULL)
    g_error ("Invalid server URL '%s' could not be parsed because: %s.",
             self->server_url, error->message);

  return ping_socket;
}

static void
schedule_upload (EmerDaemon  *self,
                 const gchar *environment)
{
  guint network_send_interval;
  if (self->network_send_interval != 0u)
    network_send_interval = self->network_send_interval;
  else if (g_strcmp0 (environment, "production") == 0)
    network_send_interval = PRODUCTION_NETWORK_SEND_INTERVAL;
  else
    network_send_interval = DEV_NETWORK_SEND_INTERVAL;

  self->upload_events_timeout_source_id =
    g_timeout_add_seconds (network_send_interval,
                           (GSourceFunc) handle_upload_timer,
                           self);
}

static gboolean
upload_permitted (EmerDaemon *self,
                  GError    **error)
{
  if (!self->recording_enabled)
    {
      g_set_error (error, EMER_ERROR, EMER_ERROR_METRICS_DISABLED,
                   METRICS_DISABLED_MESSAGE);
      return FALSE;
    }

  gboolean uploading_enabled =
    emer_permissions_provider_get_uploading_enabled (self->permissions_provider);
  if (!uploading_enabled)
    {
      flush_to_persistent_cache (self);
      g_set_error (error, EMER_ERROR, EMER_ERROR_UPLOADING_DISABLED,
                   UPLOADING_DISABLED_MESSAGE);
      return FALSE;
    }

  return TRUE;
}

static void
dequeue_and_do_upload (EmerDaemon  *self,
                       const gchar *environment)
{
  GError *error = NULL;
  if (!upload_permitted (self, &error))
    {
      GTask *upload_task = g_queue_pop_head (self->upload_queue);
      g_task_return_error (upload_task, error);
      g_object_unref (upload_task);
      g_signal_emit (self, emer_daemon_signals[SIGNAL_UPLOAD_FINISHED], 0u);
      return;
    }

  g_assert (self->server_url != NULL);
  if (strstr (self->server_url, "${environment}") != NULL)
    {
      GString *s = g_string_new (self->server_url);
      g_string_replace (s, "${environment}", environment, 0);

      g_free (self->server_url);
      self->server_url = g_string_free (s, FALSE);
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
               gsize               max_upload_size,
               const gchar        *environment,
               GAsyncReadyCallback callback,
               gpointer            user_data)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new ();
  GTask *upload_task = g_task_new (self, cancellable, callback, user_data);
  // The rest of the fields will be populated when the request is dequeued
  NetworkCallbackData *callback_data = g_new0 (NetworkCallbackData, 1);
  callback_data->max_upload_size = max_upload_size;
  g_task_set_task_data (upload_task, callback_data,
                        (GDestroyNotify) network_callback_data_free);
  g_queue_push_tail (self->upload_queue, upload_task);
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
      if (!g_error_matches (error, EMER_ERROR, EMER_ERROR_METRICS_DISABLED) &&
          !g_error_matches (error, EMER_ERROR, EMER_ERROR_UPLOADING_DISABLED) &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Failed to upload events: %s.", error->message);
        }

      g_error_free (error);
    }
}

static gboolean
handle_upload_timer (EmerDaemon *self)
{
  gchar *environment =
    emer_permissions_provider_get_environment (self->permissions_provider);
  schedule_upload (self, environment);
  upload_events (self, MAX_REQUEST_PAYLOAD, environment,
                 (GAsyncReadyCallback) log_upload_error, NULL /* user_data */);
  g_free (environment);

  return G_SOURCE_REMOVE;
}

static void
handle_upload_finished (EmerDaemon *self)
{
  if (g_queue_is_empty (self->upload_queue))
    return;

  gchar *environment =
    emer_permissions_provider_get_environment (self->permissions_provider);
  dequeue_and_do_upload (self, environment);
  g_free (environment);
}

static void
on_permissions_changed (EmerPermissionsProvider *permissions_provider,
                        GParamSpec              *pspec,
                        EmerDaemon              *self)
{
  self->recording_enabled =
    emer_permissions_provider_get_daemon_enabled (permissions_provider);

  if (!self->recording_enabled)
    {
      /* Discard any outstanding events */
      GError *error = NULL;

      remove_events (self, self->variant_array->len);
      g_hash_table_remove_all (self->monitored_senders);
      g_hash_table_remove_all (self->aggregate_timers);

      if (!emer_persistent_cache_remove_all (self->persistent_cache, &error))
        {
          g_warning ("failed to clear persistent cache: %s", error->message);
          g_clear_error (&error);
        }

      if (!emer_aggregate_tally_clear (self->aggregate_tally, &error))
        {
          g_warning ("failed to clear tally: %s", error->message);
          g_clear_error (&error);
        }

      /* If NULL (because no upload is in progress), this is a no-op. */
      g_cancellable_cancel (self->current_upload_cancellable);
    }
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
  self->rand = rand == NULL ? g_rand_new () : rand;
}

static void
set_server_url (EmerDaemon  *self,
                const gchar *server_url)
{
  g_free (self->server_url);
  self->server_url = NULL;

  if (server_url != NULL)
    self->server_url = g_build_filename (server_url, CLIENT_VERSION_NUMBER "/", NULL);
}

static void
set_network_send_interval (EmerDaemon *self,
                           guint       seconds)
{
  self->network_send_interval = seconds;
}

static void
set_permissions_provider (EmerDaemon              *self,
                          EmerPermissionsProvider *permissions_provider)
{
  if (permissions_provider == NULL)
    self->permissions_provider = emer_permissions_provider_new ();
  else
    self->permissions_provider = g_object_ref (permissions_provider);

  g_signal_connect (self->permissions_provider, "notify::daemon-enabled",
                    G_CALLBACK (on_permissions_changed), self);
  self->recording_enabled =
    emer_permissions_provider_get_daemon_enabled (self->permissions_provider);
}

static void
set_persistent_cache_directory (EmerDaemon  *self,
                                const gchar *persistent_cache_directory)
{
  self->persistent_cache_directory = g_strdup (persistent_cache_directory);
}

static void
set_persistent_cache (EmerDaemon          *self,
                      EmerPersistentCache *persistent_cache)
{
  if (persistent_cache != NULL)
    g_object_ref (persistent_cache);
  self->persistent_cache = persistent_cache;
}

static void
set_aggregate_tally (EmerDaemon         *self,
                     EmerAggregateTally *tally)
{
  g_set_object (&self->aggregate_tally, tally);
}

static void
set_max_bytes_buffered (EmerDaemon *self,
                        gsize       max_bytes_buffered)
{
  self->max_bytes_buffered = max_bytes_buffered;
}

static void schedule_next_midnight_tick (EmerDaemon *self);

static void
save_aggregate_timers_to_tally (EmerDaemon    *self,
                                EmerTallyType  tally_type,
                                GDateTime     *datetime,
                                gint64         monotonic_time_us)
{
  EmerAggregateTimerImpl *timer_impl;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, self->aggregate_timers);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &timer_impl))
    {
      g_autoptr(GError) error = NULL;

      emer_aggregate_timer_impl_store (timer_impl,
                                       tally_type,
                                       datetime,
                                       monotonic_time_us,
                                       &error);

      /* Erroring here shouldn't stop the loop */
      if (error)
        g_warning ("Error storing timer: %s", error->message);
    }
}

static void
split_aggregate_timers (EmerDaemon *self,
                        gint64      monotonic_time_us)
{
  EmerAggregateTimerImpl *timer_impl;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, self->aggregate_timers);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &timer_impl))
    emer_aggregate_timer_impl_split (timer_impl, monotonic_time_us);
}

static EmerTallyIterResult
buffer_aggregate_event_to_queue (guint32     unix_user_id,
                                 uuid_t      event_uuid,
                                 GVariant   *payload,
                                 guint32     counter,
                                 const char *date,
                                 gpointer    user_data)
{
  EmerDaemon *self = user_data;

  emer_daemon_enqueue_aggregate_event (self,
                                       get_uuid_as_variant (event_uuid),
                                       date,
                                       counter,
                                       payload);

  return EMER_TALLY_ITER_CONTINUE;
}

static inline gboolean
month_changed (GDateTime *a,
               GDateTime *b)
{
  return g_date_time_get_year (a) != g_date_time_get_year (b) ||
         g_date_time_get_month (a) != g_date_time_get_month (b);
}

static gboolean
clock_ticked_midnight_cb (gpointer user_data)
{
  EmerDaemon *self = EMER_DAEMON (user_data);
  g_autoptr (GDateTime) now = NULL;
  g_autofree char *date = NULL;
  gint64 now_monotonic_us;

  now_monotonic_us = g_get_monotonic_time ();

  /* Daily events */
  date = g_date_time_format (self->current_aggregate_tally_date,
                             "%Y-%m-%d");

  g_message ("Buffering daily aggregate events from %s to submission queue",
             date);

  save_aggregate_timers_to_tally (self,
                                  EMER_TALLY_DAILY_EVENTS,
                                  self->current_aggregate_tally_date,
                                  now_monotonic_us);

  emer_aggregate_tally_iter (self->aggregate_tally,
                             EMER_TALLY_DAILY_EVENTS,
                             self->current_aggregate_tally_date,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             buffer_aggregate_event_to_queue,
                             self);

  /* Monthly events */
  now = g_date_time_new_now_local ();
  if (month_changed (now, self->current_aggregate_tally_date))
    {
      g_autofree char *month_date = NULL;

      month_date = g_date_time_format (self->current_aggregate_tally_date,
                                       "%Y-%m");

      g_message ("Buffering monthly aggregate events from %s to submission queue",
                 month_date);

      save_aggregate_timers_to_tally (self,
                                      EMER_TALLY_MONTHLY_EVENTS,
                                      self->current_aggregate_tally_date,
                                      now_monotonic_us);

      emer_aggregate_tally_iter (self->aggregate_tally,
                                 EMER_TALLY_MONTHLY_EVENTS,
                                 self->current_aggregate_tally_date,
                                 EMER_TALLY_ITER_FLAG_DELETE,
                                 buffer_aggregate_event_to_queue,
                                 self);
    }

  split_aggregate_timers (self, now_monotonic_us);

  schedule_next_midnight_tick (self);

  return G_SOURCE_REMOVE;
}

static void
schedule_next_midnight_tick (EmerDaemon *self)
{
  g_autoptr (GDateTime) next_midnight = NULL;
  g_autoptr (GDateTime) next_day = NULL;
  g_autoptr (GDateTime) now = NULL;
  gint64 time_to_next_midnight_us;

  now = g_date_time_new_now_local ();
  next_day = g_date_time_add_days (now, 1);
  next_midnight = g_date_time_new_local (g_date_time_get_year (next_day),
                                         g_date_time_get_month (next_day),
                                         g_date_time_get_day_of_month (next_day),
                                         0, 0, 0.0);

  time_to_next_midnight_us = g_date_time_difference (next_midnight, now);

  self->dispatch_aggregate_timers_daily_source_id =
    eins_boottimeout_add_useconds (time_to_next_midnight_us,
                                   clock_ticked_midnight_cb,
                                   self);

  self->current_aggregate_tally_date = g_date_time_ref (now);
}

static void
remove_timer (EmerDaemon             *self,
              EmerAggregateTimerImpl *timer_impl)
{
  if (!g_hash_table_remove (self->aggregate_timers, timer_impl))
    {
      g_warning ("Stopped timer %p was not in the set of %d running timers",
                 timer_impl,
                 g_hash_table_size (self->aggregate_timers));
    }
}

static gboolean
on_timer_stopped_cb (EmerAggregateTimer     *timer,
                     GDBusMethodInvocation  *invocation,
                     EmerAggregateTimerImpl *timer_impl)
{
  EmerDaemon *self = g_object_get_data (G_OBJECT (timer_impl), "daemon");
  AggregateTimerSenderData *sender_data;
  g_autoptr(GDateTime) now = NULL;
  g_autoptr(GError) error = NULL;
  const gchar *sender_name;
  gint64 now_monotonic_us;

  now = g_date_time_new_now_local ();
  now_monotonic_us = g_get_monotonic_time ();
  emer_aggregate_timer_impl_stop (timer_impl, now, now_monotonic_us, &error);
  if (error)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    emer_aggregate_timer_complete_stop_timer (timer, invocation);

  /* Stop watching the sender if this was the last of its timers */
  sender_name = emer_aggregate_timer_impl_get_sender_name (timer_impl);
  sender_data = g_hash_table_lookup (self->monitored_senders, sender_name);
  g_assert (sender_data != NULL);
  g_ptr_array_remove_fast (sender_data->aggregate_timers, timer_impl);
  if (sender_data->aggregate_timers->len == 0)
    g_hash_table_remove (self->monitored_senders, sender_name);

  remove_timer (self, timer_impl);

  return TRUE;
}

static void
sender_name_vanished_cb (GDBusConnection *connection,
                         const gchar     *sender_name,
                         gpointer         user_data)
{
  EmerDaemon *self = EMER_DAEMON (user_data);
  AggregateTimerSenderData *sender_data;
  g_autoptr(GDateTime) now = NULL;
  gint64 now_monotonic_us;
  guint i;

  now = g_date_time_new_now_local ();
  now_monotonic_us = g_get_monotonic_time ();

  sender_data = g_hash_table_lookup (self->monitored_senders, sender_name);
  g_assert (sender_data != NULL);

  for (i = 0; i < sender_data->aggregate_timers->len; i++)
    {
      g_autoptr(GError) error = NULL;
      EmerAggregateTimerImpl *timer_impl =
        g_ptr_array_index (sender_data->aggregate_timers, i);

      emer_aggregate_timer_impl_stop (timer_impl,
                                      now,
                                      now_monotonic_us,
                                      &error);

      if (error)
        g_warning ("Error stopping aggregate timer after sender disappeared: %s",
                   error->message);

      remove_timer (self, timer_impl);
    }

  g_hash_table_remove (self->monitored_senders, sender_name);
}

static void
buffer_past_aggregate_events (EmerDaemon *self)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();

  emer_aggregate_tally_iter_before (self->aggregate_tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    now,
                                    EMER_TALLY_ITER_FLAG_DELETE,
                                    buffer_aggregate_event_to_queue,
                                    self);

  emer_aggregate_tally_iter_before (self->aggregate_tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    now,
                                    EMER_TALLY_ITER_FLAG_DELETE,
                                    buffer_aggregate_event_to_queue,
                                    self);

}

static void
emer_daemon_constructed (GObject *object)
{
  EmerDaemon *self = EMER_DAEMON (object);

  if (self->persistent_cache == NULL)
    {
      guint64 max_cache_size =
        emer_cache_size_provider_get_max_cache_size (NULL);
      g_autoptr(GError) error = NULL;

      self->persistent_cache =
        emer_persistent_cache_new (self->persistent_cache_directory,
                                   max_cache_size,
                                   FALSE,
                                   &error);

      if (self->persistent_cache == NULL &&
          error != NULL && error->domain == G_KEY_FILE_ERROR)
        {
          g_warning ("Persistent cache metadata in %s was corrupt: %s",
                     self->persistent_cache_directory,
                     error->message);
          g_clear_error (&error);

          g_message ("Attempting to reinitialize the persistent cache");
          self->persistent_cache =
            emer_persistent_cache_new (self->persistent_cache_directory,
                                       max_cache_size,
                                       TRUE,
                                       &error);
        }

      if (self->persistent_cache == NULL)
        g_error ("Could not create persistent cache in %s: %s.",
                 self->persistent_cache_directory, error->message);
    }

  if (self->aggregate_tally == NULL)
    {
      self->aggregate_tally =
        emer_aggregate_tally_new (self->persistent_cache_directory ?: g_get_user_cache_dir ());
    }
  buffer_past_aggregate_events (self);

  if (self->server_url == NULL)
    {
      g_autofree gchar *server_url = emer_permissions_provider_get_server_url (self->permissions_provider);
      if (server_url != NULL)
        set_server_url (self, server_url);
      else
        set_server_url (self, DEFAULT_METRICS_SERVER_URL);
    }

  gchar *environment =
    emer_permissions_provider_get_environment (self->permissions_provider);
  schedule_upload (self, environment);
  g_free (environment);

  G_OBJECT_CLASS (emer_daemon_parent_class)->constructed (object);
}

static void
emer_daemon_get_property (GObject      *object,
                          guint         property_id,
                          GValue       *value,
                          GParamSpec   *pspec)
{
  EmerDaemon *self = EMER_DAEMON (object);

  switch (property_id)
    {
    case PROP_PERSISTENT_CACHE:
      g_value_set_object (value, self->persistent_cache);
      break;

    case PROP_AGGREGATE_TALLY:
      g_value_set_object (value, self->aggregate_tally);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
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

    case PROP_SERVER_URL:
      set_server_url (self, g_value_get_string (value));
      break;

    case PROP_NETWORK_SEND_INTERVAL:
      set_network_send_interval (self, g_value_get_uint (value));
      break;

    case PROP_PERMISSIONS_PROVIDER:
      set_permissions_provider (self, g_value_get_object (value));
      break;

    case PROP_PERSISTENT_CACHE_DIRECTORY:
      set_persistent_cache_directory (self, g_value_get_string (value));
      break;

    case PROP_PERSISTENT_CACHE:
      set_persistent_cache (self, g_value_get_object (value));
      break;

    case PROP_AGGREGATE_TALLY:
      set_aggregate_tally (self, g_value_get_object (value));
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

  /* While an upload is ongoing, the GTask holds a ref to the EmerDaemon. */
  g_warn_if_fail (self->current_upload_cancellable == NULL);

  g_clear_pointer (&self->monitored_senders, g_hash_table_destroy);
  g_clear_pointer (&self->aggregate_timers, g_hash_table_destroy);

  g_source_remove (self->upload_events_timeout_source_id);

  if (self->report_invalid_cache_data_source_id != 0)
    g_source_remove (self->report_invalid_cache_data_source_id);

  if (self->dispatch_aggregate_timers_daily_source_id != 0)
    g_source_remove (self->dispatch_aggregate_timers_daily_source_id);

  g_clear_pointer (&self->current_aggregate_tally_date, g_date_time_unref);

  flush_to_persistent_cache (self);
  g_clear_object (&self->persistent_cache);

  g_queue_free_full (self->upload_queue, g_object_unref);

  soup_session_abort (self->http_session);
  g_clear_object (&self->http_session);

  g_clear_pointer (&self->variant_array, g_ptr_array_unref);

  g_rand_free (self->rand);
  g_clear_pointer (&self->server_url, g_free);
  g_clear_object (&self->permissions_provider);
  g_clear_object (&self->aggregate_tally);
  g_clear_pointer (&self->persistent_cache_directory, g_free);

  G_OBJECT_CLASS (emer_daemon_parent_class)->finalize (object);
}

static void
emer_daemon_class_init (EmerDaemonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = emer_daemon_constructed;
  object_class->get_property = emer_daemon_get_property;
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
   * EmerDaemon:server-url:
   *
   * The URL to which events are uploaded. The URL must contain the protocol and
   * may contain the port number. If unspecified, the port number defaults to
   * 443, which is the standard port number for SSL.
   */
  emer_daemon_props[PROP_SERVER_URL] =
    g_param_spec_string ("server-url", "Server URL",
                         "URL to which events are uploaded",
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
                       "Number of seconds between attempts to upload events to "
                       "server",
                       0, G_MAXUINT, 0,
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
   * EmerDaemon:persistent-cache-directory:
   *
   * A directory for temporarily storing events until they are uploaded to the
   * metrics servers. If no directory is specified, a default directory is used.
   */
  emer_daemon_props[PROP_PERSISTENT_CACHE_DIRECTORY] =
    g_param_spec_string ("persistent-cache-directory",
                         "Persistent cache directory",
                         "The directory in which to temporarily store events "
                         "locally",
                         NULL,
                         G_PARAM_CONSTRUCT | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);


  /*
   * EmerDaemon:persistent-cache:
   *
   * An #EmerPersistentCache for storing events until they are uploaded to the
   * metrics servers.
   * If this property is not specified, a default persistent cache (created by
   * emer_persistent_cache_new ()) will be used.
   */
  emer_daemon_props[PROP_PERSISTENT_CACHE] =
    g_param_spec_object ("persistent-cache", "Persistent cache",
                         "Object managing persistent storage of events until "
                         "they are uploaded to the metrics servers",
                         EMER_TYPE_PERSISTENT_CACHE,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                         G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:aggregate-tally: (nullable)
   *
   * An #EmerAggregateTally for tallying up aggregates until the period ends
   * and they are enqueued for upload.
   * If this property is not specified, a default will be created with
   * emer_aggregate_tally_new().
   */
  emer_daemon_props[PROP_AGGREGATE_TALLY] =
    g_param_spec_object ("aggregate-tally", "Aggregate tally",
                         "Tallies up aggregate events until they are enqueued",
                         EMER_TYPE_AGGREGATE_TALLY,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
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

  emer_daemon_signals[SIGNAL_UPLOAD_FINISHED] =
    g_signal_new_class_handler ("upload-finished",
                                EMER_TYPE_DAEMON,
                                G_SIGNAL_RUN_FIRST,
                                G_CALLBACK (handle_upload_finished),
                                NULL /* GSignalAccumulator */,
                                NULL /* accumulator_data */,
                                g_cclosure_marshal_VOID__VOID,
                                G_TYPE_NONE,
                                0u);
}

static void
emer_daemon_init (EmerDaemon *self)
{
  self->upload_queue = g_queue_new ();

  self->http_session =
    soup_session_new_with_options ("max-conns", 1,
                                   "max-conns-per-host", 1,
                                   NULL);
  soup_session_add_feature_by_type (self->http_session, SOUP_TYPE_CACHE);

  self->aggregate_timers =
    g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);

  self->monitored_senders =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                           (GDestroyNotify)aggregate_timer_sender_data_free);

  self->variant_array =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_variant_unref);

  /* Start aggregate timers now so it can buffer previously stored events */
  schedule_next_midnight_tick (self);
}

/*
 * emer_daemon_new:
 *
 * Returns: (transfer full): a new #EmerDaemon with the default configuration.
 */
EmerDaemon *
emer_daemon_new (const gchar             *persistent_cache_directory,
                 EmerPermissionsProvider *permissions_provider)
{
  return g_object_new (EMER_TYPE_DAEMON,
                       "persistent-cache-directory", persistent_cache_directory,
                       "permissions-provider", permissions_provider,
                       NULL);
}

/*
 * emer_daemon_new_full:
 * @rand: (allow-none): random number generator to use for randomized
 *   exponential backoff, or %NULL to use the default.
 * @server_url: (allow-none): the URL (including protocol and, optionally, port
 *   number) to which to upload events, or %NULL to use the default. Must
 *   include trailing forward slash. If the port number is unspecified, it
 *   defaults to 443 (the standard port used by SSL).
 * @network_send_interval: frequency in seconds with which the client will
 *   attempt a network send request.
 * @permissions_provider: The #EmerPermissionsProvider to supply information
 *   about opting out of metrics collection, disabling network uploads, and the
 *   metrics environment (dev or production).
 * @persistent_cache: (allow-none): The #EmerPersistentCache in which to store
 *   metrics locally when they can't be sent over the network, or %NULL to use
 *   the default.
 * @aggregate_tally: (allow-none): The #EmerAggregateTally in which to tally
 *   aggregate events until they can be enqueued, or %NULL to use
 *   the default.
 * @max_bytes_buffered: The maximum number of bytes of event data that may be
 *   stored in memory. Does not include overhead.
 *
 * Returns: (transfer full): a new customized #EmerDaemon. Use emer_daemon_new
 * to use the default configuration.
 */
EmerDaemon *
emer_daemon_new_full (GRand                   *rand,
                      const gchar             *server_url,
                      guint                    network_send_interval,
                      EmerPermissionsProvider *permissions_provider,
                      EmerPersistentCache     *persistent_cache,
                      EmerAggregateTally      *aggregate_tally,
                      gulong                   max_bytes_buffered)
{
  return g_object_new (EMER_TYPE_DAEMON,
                       "random-number-generator", rand,
                       "server-url", server_url,
                       "network-send-interval", network_send_interval,
                       "permissions-provider", permissions_provider,
                       "persistent-cache", persistent_cache,
                       "aggregate-tally", aggregate_tally,
                       "max-bytes-buffered", max_bytes_buffered,
                       NULL);
}

void
emer_daemon_record_singular_event (EmerDaemon *self,
                                   GVariant   *event_id,
                                   gint64      relative_timestamp,
                                   gboolean    has_payload,
                                   GVariant   *payload)
{
  g_return_if_fail (g_variant_is_of_type (payload, G_VARIANT_TYPE_VARIANT));

  g_autofree gchar *os_version = emer_image_id_provider_get_os_version();

  if (!self->recording_enabled)
    return;

  if (!is_uuid (event_id))
    {
      g_warning ("Event ID must be a UUID represented as an array of %"
                 G_GSIZE_FORMAT " bytes. Dropping event.", UUID_LENGTH);
      return;
    }

  gint64 boot_offset;
  GError *error = NULL;
  if (!emer_persistent_cache_get_boot_time_offset (self->persistent_cache,
                                                   &boot_offset, &error))
    {
      g_warning ("Unable to correct event's relative timestamp. Dropping "
                 "event. Error: %s.", error->message);
      g_error_free (error);
      return;
    }
  relative_timestamp += boot_offset;

  GVariant *nullable_payload = get_nullable_payload (payload, has_payload);
  GVariant *singular =
    g_variant_new ("(@aysxm@v)", event_id, os_version, relative_timestamp,
                   nullable_payload);
  buffer_event (self, singular);
}

void
emer_daemon_enqueue_aggregate_event (EmerDaemon *self,
                                     GVariant   *event_id,
                                     const char *period_start,
                                     guint32     count,
                                     GVariant   *payload)
{
  g_return_if_fail (payload == NULL || g_variant_is_of_type (payload, G_VARIANT_TYPE_VARIANT));

  g_autofree gchar *os_version = emer_image_id_provider_get_os_version();

  if (!self->recording_enabled)
    return;

  if (!is_uuid (event_id))
    {
      g_warning ("Event ID must be a UUID represented as an array of %"
                 G_GSIZE_FORMAT " bytes. Dropping event.", UUID_LENGTH);
      return;
    }

  GVariant *aggregate =
    g_variant_new ("(@ayssum@v)", event_id, os_version, period_start, count,
                   payload);
  buffer_event (self, aggregate);
}

void
emer_daemon_record_event_sequence (EmerDaemon *self,
                                   guint32     user_id,
                                   GVariant   *event_id,
                                   GVariant   *event_values)
{
}

/* emer_daemon_upload_events:
 * @self: the daemon
 * @callback: (nullable): the function to call once the upload completes. The
 * first parameter passed to this callback is self. The second parameter is a
 * GAsyncResult that can be passed to emer_daemon_upload_events_finish to
 * determine whether the upload succeeded. The third parameter is user_data.
 * @user_data: (nullable): arbitrary data that is blindly passed through to the
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

  gchar *environment =
    emer_permissions_provider_get_environment (self->permissions_provider);
  upload_events (self, G_MAXSIZE, environment, callback, user_data);
  g_free (environment);
}

/* emer_daemon_upload_events_finish:
 * @self: the daemon
 * @result: a GAsyncResult that encapsulates whether the upload succeeded
 * @error: (out) (optional): if the upload failed, error will be set to a GError
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
  return self->permissions_provider;
}

gboolean
emer_daemon_start_aggregate_timer (EmerDaemon       *self,
                                   GDBusConnection  *connection,
                                   const gchar      *sender_name,
                                   guint32           unix_user_id,
                                   GVariant         *event_id,
                                   gboolean          has_payload,
                                   GVariant         *payload,
                                   gchar           **out_timer_object_path,
                                   GError          **error)
{
  EmerAggregateTimerImpl *timer_impl;
  AggregateTimerSenderData *sender_data;
  GVariant *nullable_payload;

  g_return_val_if_fail (EMER_IS_DAEMON (self), FALSE);
  g_return_val_if_fail (event_id != NULL, FALSE);

  if (!self->recording_enabled)
    {
      g_set_error (error, EMER_ERROR, EMER_ERROR_METRICS_DISABLED,
                   METRICS_DISABLED_MESSAGE);
      return FALSE;
    }

  if (!is_uuid (event_id))
    {
      g_set_error (error, EMER_ERROR, EMER_ERROR_INVALID_EVENT_ID,
                   "Event ID must be a UUID represented as an array of %"
                   G_GSIZE_FORMAT " bytes. Dropping event.",
                   UUID_LENGTH);
      return FALSE;
    }

  nullable_payload = get_nullable_payload (payload, has_payload);

  g_autofree gchar *timer_object_path = NULL;
  g_autoptr(EmerAggregateTimer) timer = NULL;
  g_autoptr(GError) local_error = NULL;
  static guint64 timer_id = 0;

  timer_object_path = g_strdup_printf ("/com/endlessm/Metrics/AggregateTimer%lu",
                                       timer_id);

  timer = emer_aggregate_timer_skeleton_new ();
  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (timer),
                                    connection,
                                    timer_object_path,
                                    &local_error);
  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  // Only increment on success, otherwise we waste ids for nothing
  timer_id++;

  timer_impl = emer_aggregate_timer_impl_new (self->aggregate_tally,
                                              g_object_ref (timer),
                                              sender_name,
                                              unix_user_id,
                                              event_id,
                                              nullable_payload,
                                              g_get_monotonic_time ());
  g_object_set_data_full (G_OBJECT (timer_impl),
                          "daemon",
                          g_object_ref (self),
                          g_object_unref);

  g_signal_connect (timer,
                    "handle-stop-timer",
                    G_CALLBACK (on_timer_stopped_cb),
                    timer_impl);

  /* Monitor sender and shut down all timers from it when it vanishes */
  sender_data = g_hash_table_lookup (self->monitored_senders, sender_name);
  if (!sender_data)
    {
      sender_data = g_new0 (AggregateTimerSenderData, 1);
      sender_data->aggregate_timers = g_ptr_array_sized_new (1);
      sender_data->watch_id =
        g_bus_watch_name_on_connection (connection,
                                        sender_name,
                                        G_BUS_NAME_WATCHER_FLAGS_NONE,
                                        NULL,
                                        sender_name_vanished_cb,
                                        self,
                                        NULL);
      g_hash_table_insert (self->monitored_senders,
                           g_strdup (sender_name),
                           sender_data);
    }

  g_ptr_array_add (sender_data->aggregate_timers, timer_impl);
  g_hash_table_add (self->aggregate_timers, timer_impl);

  *out_timer_object_path = g_steal_pointer (&timer_object_path);

  return TRUE;
}

void
emer_daemon_shutdown (EmerDaemon  *self)
{
  g_autoptr(GDateTime) now = NULL;
  gint64 now_monotonic_us;
  GHashTableIter iter;
  gpointer value;

  g_return_if_fail (EMER_IS_DAEMON (self));

  now = g_date_time_new_now_local ();
  now_monotonic_us = g_get_monotonic_time ();

  g_hash_table_iter_init (&iter, self->aggregate_timers);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      EmerAggregateTimerImpl *timer_impl = EMER_AGGREGATE_TIMER_IMPL (value);
      g_autoptr(GError) local_error = NULL;

      if (!emer_aggregate_timer_impl_stop (timer_impl, now, now_monotonic_us, &local_error))
        g_warning ("Failed to stop timer: %s", local_error->message);

      g_hash_table_iter_remove (&iter);
    }

  g_assert (g_hash_table_size (self->aggregate_timers) == 0);
  g_hash_table_remove_all (self->monitored_senders);
}

