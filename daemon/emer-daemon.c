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

#include <string.h>
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

/*
 * This is the default maximum length for each event buffer. It is set based on
 * the assumption that events average 100 bytes each. The desired maximum
 * capacity of all three event buffers combined is 100 kB. Thus,
 * (10,2400 bytes) / ((100 bytes / event) * 3 buffers) ~= 341 events / buffer.
 *
 * TODO: Use the actual size of the events to determine the limit, not the
 * estimated size.
 */
#define DEFAULT_METRICS_PER_BUFFER_MAX 341

#define WHAT "shutdown"
#define WHO "EndlessOS Event Recorder Daemon"
#define WHY "Flushing events to disk"
#define MODE "delay"
#define INHIBIT_ARGS "('" WHAT "', '" WHO "', '" WHY "', '" MODE "')"

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

  /* Storage buffers for different event types */

  SingularEvent * volatile singular_buffer;
  gint singular_buffer_length;
  volatile gint num_singulars_buffered;

  AggregateEvent * volatile aggregate_buffer;
  gint aggregate_buffer_length;
  volatile gint num_aggregates_buffered;

  SequenceEvent * volatile sequence_buffer;
  gint sequence_buffer_length;
  volatile gint num_sequences_buffered;
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
  PROP_SINGULAR_BUFFER_LENGTH,
  PROP_AGGREGATE_BUFFER_LENGTH,
  PROP_SEQUENCE_BUFFER_LENGTH,
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
uuid_from_variant (GVariant *variant,
                   uuid_t    uuid)
{
  g_variant_ref_sink (variant);
  gsize variant_length;
  const guchar *array =
    g_variant_get_fixed_array (variant, &variant_length, sizeof (guchar));
  uuid_copy (uuid, array);
  g_variant_unref (variant);
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
                                                   &boot_offset, error, FALSE))
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
  g_variant_get (request_body, "(ixx@ay@a(uayxmv)@a(uayxxmv)@a(uaya(xmv)))",
                 &send_number, NULL /* relative time */,
                 NULL /* absolute time */, &machine_id, &singulars, &aggregates,
                 &sequences);

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

  return g_variant_new ("(ixx@ay@a(uayxmv)@a(uayxxmv)@a(uaya(xmv)))",
                        send_number, little_endian_relative_timestamp,
                        little_endian_absolute_timestamp, machine_id,
                        singulars, aggregates, sequences);
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
drain_singulars_buffer (EmerDaemonPrivate *priv,
                        GVariantBuilder   *singulars_builder)
{
  SingularEvent *singular_buffer = priv->singular_buffer;
  gint num_singulars = priv->num_singulars_buffered;
  for (gint i = 0; i < num_singulars; ++i)
    {
      SingularEvent *curr_singular = singular_buffer + i;
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_singular->event_id, &event_id_builder);
      EventValue curr_event_value = curr_singular->event_value;
      g_variant_builder_add (singulars_builder, "(uayxmv)",
                             curr_singular->user_id,
                             &event_id_builder,
                             curr_event_value.relative_timestamp,
                             curr_event_value.auxiliary_payload);
      trash_singular_event (curr_singular);
    }
  priv->num_singulars_buffered = 0;
}

static void
drain_aggregates_buffer (EmerDaemonPrivate *priv,
                         GVariantBuilder   *aggregates_builder)
{
  AggregateEvent *aggregate_buffer = priv->aggregate_buffer;
  gint num_aggregates = priv->num_aggregates_buffered;
  for (gint i = 0; i < num_aggregates; ++i)
    {
      AggregateEvent *curr_aggregate = aggregate_buffer + i;
      SingularEvent curr_event = curr_aggregate->event;
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_event.event_id, &event_id_builder);
      EventValue curr_event_value = curr_event.event_value;
      g_variant_builder_add (aggregates_builder, "(uayxxmv)",
                             curr_event.user_id,
                             &event_id_builder,
                             curr_aggregate->num_events,
                             curr_event_value.relative_timestamp,
                             curr_event_value.auxiliary_payload);
      trash_aggregate_event (curr_aggregate);
    }
  priv->num_aggregates_buffered = 0;
}

static void
drain_sequences_buffer (EmerDaemonPrivate *priv,
                        GVariantBuilder   *sequences_builder)
{
  SequenceEvent *sequence_buffer = priv->sequence_buffer;
  gint num_sequences = priv->num_sequences_buffered;
  for (gint i = 0; i < num_sequences; ++i)
    {
      SequenceEvent *curr_sequence = sequence_buffer + i;

      GVariantBuilder event_values_builder;
      g_variant_builder_init (&event_values_builder, G_VARIANT_TYPE ("a(xmv)"));
      for (gint j = 0; j < curr_sequence->num_event_values; ++j)
        {
          EventValue *curr_event_value = curr_sequence->event_values + j;
          g_variant_builder_add (&event_values_builder, "(xmv)",
                                 curr_event_value->relative_timestamp,
                                 curr_event_value->auxiliary_payload);
        }

      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_sequence->event_id, &event_id_builder);

      g_variant_builder_add (sequences_builder, "(uaya(xmv))",
                             curr_sequence->user_id, &event_id_builder,
                             &event_values_builder);
      trash_sequence_event (curr_sequence);
    }
  priv->num_sequences_buffered = 0;
}

/*
 * Populates the given GVariantBuilders with metric events from the persistent
 * cache. Returns TRUE on success even if there were no events found in the
 * persistent cache. Returns FALSE on failure. Regardless of success of failure,
 * the GVariantBuilders are guaranteed to be correctly initialized.
 */
static gboolean
drain_persistent_cache (EmerDaemonPrivate *priv,
                        GVariantBuilder   *singulars_builder,
                        GVariantBuilder   *aggregates_builder,
                        GVariantBuilder   *sequences_builder)
{
  g_variant_builder_init (singulars_builder, G_VARIANT_TYPE ("a(uayxmv)"));
  g_variant_builder_init (aggregates_builder, G_VARIANT_TYPE ("a(uayxxmv)"));
  g_variant_builder_init (sequences_builder, G_VARIANT_TYPE ("a(uaya(xmv))"));

  GVariant **list_of_singulars;
  GVariant **list_of_aggregates;
  GVariant **list_of_sequences;

  /*
   * TODO: This value is currently unused by the persistent cache, but should be
   * updated to a sensible value when the PC starts using it.
   */
  gint maximum_bytes_to_drain = 92160;
  if (!emer_persistent_cache_drain_metrics (priv->persistent_cache,
                                            &list_of_singulars,
                                            &list_of_aggregates,
                                            &list_of_sequences,
                                            maximum_bytes_to_drain))
    return FALSE;

  for (gint i = 0; list_of_singulars[i] != NULL; i++)
    g_variant_builder_add_value (singulars_builder, list_of_singulars[i]);
  g_free (list_of_singulars);

  for (gint i = 0; list_of_aggregates[i] != NULL; i++)
    g_variant_builder_add_value (aggregates_builder, list_of_aggregates[i]);
  g_free (list_of_aggregates);

  for (gint i = 0; list_of_sequences[i] != NULL; i++)
    g_variant_builder_add_value (sequences_builder, list_of_sequences[i]);
  g_free (list_of_sequences);

  return TRUE;
}

static GVariant *
create_request_body (EmerDaemon *self,
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

  GVariantBuilder singulars_builder;
  GVariantBuilder aggregates_builder;
  GVariantBuilder sequences_builder;

  // We don't care whether this failed or not and don't check the return value.
  drain_persistent_cache (priv, &singulars_builder, &aggregates_builder,
                          &sequences_builder);

  drain_singulars_buffer (priv, &singulars_builder);
  drain_aggregates_buffer (priv, &aggregates_builder);
  drain_sequences_buffer (priv, &sequences_builder);

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_offset_timestamps (self, &relative_timestamp, &absolute_timestamp,
                              error))
    return NULL;

  GVariant *request_body =
    g_variant_new ("(ixxaya(uayxmv)a(uayxxmv)a(uaya(xmv)))", send_number,
                   relative_timestamp, absolute_timestamp, &machine_id_builder,
                   &singulars_builder, &aggregates_builder, &sequences_builder);

  g_variant_ref_sink (request_body);
  GVariant *little_endian_request_body =
    swap_bytes_if_big_endian (request_body);
  g_variant_unref (request_body);

  return little_endian_request_body;
}

static GVariant *
get_nullable_payload (gboolean  has_payload,
                      GVariant *payload)
{
  g_variant_ref_sink (payload);

  if (!has_payload)
    {
      g_variant_unref (payload);
      return NULL;
    }

  return payload;
}

/*
 * Trashes the first num_singulars_stored events in the singular buffer and
 * updates the number of singulars buffered accordingly. Slides the remaining
 * events over to the beginning of the buffer. Does not free the singular buffer
 * itself.
 */
static void
trash_stored_singulars (EmerDaemon *self,
                        gint        num_singulars_stored)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  for (gint i = 0; i < num_singulars_stored; i++)
    trash_singular_event (priv->singular_buffer + i);

  priv->num_singulars_buffered -= num_singulars_stored;
  memmove (priv->singular_buffer, priv->singular_buffer + num_singulars_stored,
           sizeof (SingularEvent) * priv->num_singulars_buffered);
}

/*
 * Behaves like trash_stored_singulars() but for aggregates rather than
 * singulars.
 */
static void
trash_stored_aggregates (EmerDaemon *self,
                         gint        num_aggregates_stored)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  for (gint i = 0; i < num_aggregates_stored; i++)
    trash_aggregate_event (priv->aggregate_buffer + i);

  priv->num_aggregates_buffered -= num_aggregates_stored;
  memmove (priv->aggregate_buffer,
           priv->aggregate_buffer + num_aggregates_stored,
           sizeof (AggregateEvent) * priv->num_aggregates_buffered);
}

/*
 * Behaves like trash_stored_singulars() but for sequences rather than
 * singulars.
 */
static void
trash_stored_sequences (EmerDaemon *self,
                        gint        num_sequences_stored)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  for (gint i = 0; i < num_sequences_stored; i++)
    trash_sequence_event (priv->sequence_buffer + i);

  priv->num_sequences_buffered -= num_sequences_stored;
  memmove (priv->sequence_buffer, priv->sequence_buffer + num_sequences_stored,
           sizeof (SequenceEvent) * priv->num_sequences_buffered);
}

static void
flush_to_persistent_cache (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (!priv->recording_enabled)
    return;

  gint num_singulars_stored, num_aggregates_stored, num_sequences_stored;
  capacity_t capacity;
  emer_persistent_cache_store_metrics (priv->persistent_cache,
                                       priv->singular_buffer,
                                       priv->aggregate_buffer,
                                       priv->sequence_buffer,
                                       priv->num_singulars_buffered,
                                       priv->num_aggregates_buffered,
                                       priv->num_sequences_buffered,
                                       &num_singulars_stored,
                                       &num_aggregates_stored,
                                       &num_sequences_stored,
                                       &capacity);

  trash_stored_singulars (self, num_singulars_stored);
  trash_stored_aggregates (self, num_aggregates_stored);
  trash_stored_sequences (self, num_sequences_stored);
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

  GVariant *request_body = create_request_body (self, &error);
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
check_and_upload_events (EmerDaemon         *self,
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
  check_and_upload_events (self, environment,
                           (GAsyncReadyCallback) log_upload_error,
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
                                                   NULL, &error, TRUE))
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
  if (strcmp ("PrepareForShutdown", signal_name) == 0)
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
        emer_persistent_cache_new (NULL /* GCancellable */, &error);
      if (priv->persistent_cache == NULL)
        {
          g_error_free (error);
        }
    }
  else
    {
      priv->persistent_cache = g_object_ref (persistent_cache);
    }
}

static void
set_singular_buffer_length (EmerDaemon *self,
                            gint        length)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->singular_buffer_length = length;
  priv->singular_buffer = g_new (SingularEvent, length);
}

static void
set_aggregate_buffer_length (EmerDaemon *self,
                             gint        length)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->aggregate_buffer_length = length;
  priv->aggregate_buffer = g_new (AggregateEvent, length);
}

static void
set_sequence_buffer_length (EmerDaemon *self,
                            gint        length)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->sequence_buffer_length = length;
  priv->sequence_buffer = g_new (SequenceEvent, length);
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

    case PROP_SINGULAR_BUFFER_LENGTH:
      set_singular_buffer_length (self, g_value_get_int (value));
      break;

    case PROP_AGGREGATE_BUFFER_LENGTH:
      set_aggregate_buffer_length (self, g_value_get_int (value));
      break;

    case PROP_SEQUENCE_BUFFER_LENGTH:
      set_sequence_buffer_length (self, g_value_get_int (value));
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

  g_rand_free (priv->rand);
  g_free (priv->server_uri);
  g_clear_object (&priv->machine_id_provider);
  g_clear_object (&priv->network_send_provider);
  g_clear_object (&priv->permissions_provider);

  free_singular_buffer (priv->singular_buffer, priv->num_singulars_buffered);
  free_aggregate_buffer (priv->aggregate_buffer, priv->num_aggregates_buffered);
  free_sequence_buffer (priv->sequence_buffer, priv->num_sequences_buffered);

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
  emer_daemon_props[PROP_SINGULAR_BUFFER_LENGTH] =
    g_param_spec_int ("singular-buffer-length", "Buffer length singular",
                       "The number of events allowed to be stored in the "
                       "individual metric buffer",
                       -1, G_MAXINT, DEFAULT_METRICS_PER_BUFFER_MAX,
                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                       G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_AGGREGATE_BUFFER_LENGTH] =
    g_param_spec_int ("aggregate-buffer-length", "Buffer length aggregate",
                      "The number of events allowed to be stored in the "
                      "aggregate metric buffer",
                      -1, G_MAXINT, DEFAULT_METRICS_PER_BUFFER_MAX,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                      G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_SEQUENCE_BUFFER_LENGTH] =
    g_param_spec_int ("sequence-buffer-length", "Buffer length sequence",
                      "The number of events allowed to be stored in the "
                      "sequence metric buffer",
                      -1, G_MAXINT, DEFAULT_METRICS_PER_BUFFER_MAX,
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
 * @buffer_length: The maximum number of events to store in each of the
 *   in-memory buffers. There are three in-memory buffers, one for singulars,
 *   for aggregates, and one for sequences.
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
                      gint                     buffer_length)
{
  return g_object_new (EMER_TYPE_DAEMON,
                       "random-number-generator", rand,
                       "server-uri", server_uri,
                       "network-send-interval", network_send_interval,
                       "machine-id-provider", machine_id_provider,
                       "network-send-provider", network_send_provider,
                       "permissions-provider", permissions_provider,
                       "persistent-cache", persistent_cache,
                       "singular-buffer-length", buffer_length,
                       "aggregate-buffer-length", buffer_length,
                       "sequence-buffer-length", buffer_length,
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
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, NULL, FALSE))
    {
      g_warning ("Unable to correct event's relative timestamp. Dropping "
                 "event.");
      return;
    }
  relative_timestamp += boot_offset;

  if (priv->num_singulars_buffered < priv->singular_buffer_length)
    {
      SingularEvent *singular = priv->singular_buffer +
        priv->num_singulars_buffered;
      priv->num_singulars_buffered++;
      singular->user_id = user_id;
      uuid_from_variant (event_id, singular->event_id);
      GVariant *nullable_payload = get_nullable_payload (has_payload, payload);
      EventValue event_value = { relative_timestamp, nullable_payload };
      singular->event_value = event_value;
    }
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
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, NULL, FALSE))
    {
      g_warning ("Unable to correct event's relative timestamp. Dropping "
                 "event.");
      return;
    }
  relative_timestamp += boot_offset;

  if (priv->num_aggregates_buffered < priv->aggregate_buffer_length)
    {
      AggregateEvent *aggregate = priv->aggregate_buffer +
        priv->num_aggregates_buffered;
      priv->num_aggregates_buffered++;
      SingularEvent singular;
      singular.user_id = user_id;
      uuid_from_variant (event_id, singular.event_id);
      aggregate->num_events = num_events;
      GVariant *nullable_payload = get_nullable_payload (has_payload, payload);
      EventValue event_value = { relative_timestamp, nullable_payload };
      singular.event_value = event_value;
      aggregate->event = singular;
    }
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
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, NULL, FALSE))
    {
      g_warning ("Unable to correct event's relative timestamp. Dropping "
                 "event.");
      return;
    }

  if (priv->num_sequences_buffered < priv->sequence_buffer_length)
    {
      SequenceEvent *event_sequence =
        priv->sequence_buffer + priv->num_sequences_buffered;
      priv->num_sequences_buffered++;

      event_sequence->user_id = user_id;
      uuid_from_variant (event_id, event_sequence->event_id);

      g_variant_ref_sink (event_values);
      gsize num_events = g_variant_n_children (event_values);
      event_sequence->event_values = g_new (EventValue, num_events);
      for(gsize index = 0; index < num_events; index++)
        {
          gint64 relative_timestamp;
          gboolean has_payload;
          GVariant *maybe_payload;

          g_variant_get_child (event_values, index, "(xbv)",
                               &relative_timestamp, &has_payload,
                               &maybe_payload);

          relative_timestamp += boot_offset;
          GVariant *nullable_payload = has_payload ? maybe_payload : NULL;
          EventValue event_value = { relative_timestamp, nullable_payload };
          event_sequence->event_values[index] = event_value;
        }

      g_variant_unref (event_values);
      event_sequence->num_event_values = num_events;
    }
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
  check_and_upload_events (self, environment, callback, user_data);
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
