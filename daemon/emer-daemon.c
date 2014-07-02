/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

/* For CLOCK_BOOTTIME */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L
#error "This code requires _POSIX_C_SOURCE to be 200112L or later."
#endif

#include <byteswap.h>
#include <string.h>
#include <time.h>

#include <gio/gunixfdlist.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <uuid/uuid.h>

#include "emer-daemon.h"
#include "emer-machine-id-provider.h"
#include "emer-permissions-provider.h"
#include "emer-persistent-cache.h"
#include "shared/metrics-util.h"

/* The URI of the metrics production proxy server. Is kept in a #define to
prevent testing code from sending metrics to the production server. */
#define PRODUCTION_SERVER_URI "https://metrics.endlessm.com/"

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
 * The default maximum length for each event buffer. Set based on the assumption
 * that events average 100 bytes each. The desired maximum capacity of all three
 * event buffers combined is 10 kB. Thus,
 * (10,240 bytes) / (100 bytes * 3 buffers) ~= 34 bytes / buffer.
 *
 * TODO: Set these limits to use the actual size of the metrics to
 * determine the limit, not the estimated size.
 */
#define DEFAULT_METRICS_PER_BUFFER_MAX 34

#define WHAT "shutdown"
#define WHO "EndlessOS Event Recorder Daemon"
#define WHY "Flushing events to disk"
#define MODE "delay"
#define INHIBIT_ARGS "('" WHAT "', '" WHO "', '" WHY "', '" MODE "')"

typedef struct _NetworkCallbackData
{
  EmerDaemon *daemon;
  GVariant *request_body;
  gint attempt_num;
} NetworkCallbackData;

typedef struct _EmerDaemonPrivate {
  gint shutdown_inhibitor;
  GDBusProxy *login_manager_proxy;

  /* Private storage for public properties */

  GRand *rand;

  gint client_version_number;

  guint network_send_interval_seconds;
  guint upload_events_timeout_source_id;

  SoupSession *http_session;
  gchar *proxy_server_uri;

  EmerMachineIdProvider *machine_id_provider;
  EmerPermissionsProvider *permissions_provider;

  EmerPersistentCache *persistent_cache;

  GNetworkMonitor *network_monitor;
  GSocketConnectable *ping_socket;

  gboolean recording_enabled;

  /* Storage buffers for different event types */

  SingularEvent * volatile singular_buffer;
  gint singular_buffer_length;
  volatile gint num_singulars_buffered;
  GMutex singular_buffer_lock;

  AggregateEvent * volatile aggregate_buffer;
  gint aggregate_buffer_length;
  volatile gint num_aggregates_buffered;
  GMutex aggregate_buffer_lock;

  SequenceEvent * volatile sequence_buffer;
  gint sequence_buffer_length;
  volatile gint num_sequences_buffered;
  GMutex sequence_buffer_lock;
} EmerDaemonPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerDaemon, emer_daemon, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_RANDOM_NUMBER_GENERATOR,
  PROP_CLIENT_VERSION_NUMBER,
  PROP_NETWORK_SEND_INTERVAL,
  PROP_PROXY_SERVER_URI,
  PROP_MACHINE_ID_PROVIDER,
  PROP_PERMISSIONS_PROVIDER,
  PROP_PERSISTENT_CACHE,
  PROP_SINGULAR_BUFFER_LENGTH,
  PROP_AGGREGATE_BUFFER_LENGTH,
  PROP_SEQUENCE_BUFFER_LENGTH,
  NPROPS
};

static GParamSpec *emer_daemon_props[NPROPS] = { NULL, };

static void
free_network_callback_data (NetworkCallbackData *callback_data)
{
  g_variant_unref (callback_data->request_body);
  g_free (callback_data);
}

static gint64
swap_bytes_64_if_big_endian (gint64 value)
{
  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    return bswap_64 (value);
  if (G_BYTE_ORDER != G_LITTLE_ENDIAN)
    g_error ("This machine is neither big endian nor little endian. Mixed "
             "endian machines are not supported by the metrics system.");
  return value;
}

/*
 * Populates builder with the elements from iter. Assumes all elements are of
 * the given type.
 */
static void
get_builder_from_iter (GVariantIter       *iter,
                       GVariantBuilder    *builder,
                       const GVariantType *type)
{
  g_variant_builder_init (builder, type);
  while (TRUE)
    {
      GVariant *curr_elem = g_variant_iter_next_value (iter);
      if (curr_elem == NULL)
        break;
      g_variant_builder_add_value (builder, curr_elem);
      g_variant_unref (curr_elem);
    }
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

static void
uuid_from_gvariant (GVariant *event_id,
                    uuid_t    uuid)
{
  gsize event_id_length;
  g_variant_ref_sink (event_id);
  gconstpointer event_id_arr =
    g_variant_get_fixed_array (event_id, &event_id_length, sizeof (guchar));
  if (event_id_length != UUID_LENGTH)
    g_critical ("The event ID should be %d bytes, but it was %d. This is "
                "probably a bug in the metrics daemon.",
                UUID_LENGTH, event_id_length);
  if (event_id_length >= UUID_LENGTH)
    {
      memcpy (uuid, event_id_arr, UUID_LENGTH * sizeof (guchar));
    }
  else
    {
      memcpy (uuid, event_id_arr, event_id_length * sizeof (guchar));
      memset (uuid + event_id_length, '\0',
              (UUID_LENGTH - event_id_length) * sizeof (guchar));
    }
  g_variant_unref (event_id);
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

/* Returned object is owned by calling code. Free with soup_uri_free() when
done. */
static SoupURI *
get_https_request_uri (EmerDaemon   *self,
                       const guchar *data,
                       gsize         length)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  gchar *checksum_string = g_compute_checksum_for_data (G_CHECKSUM_SHA512, data,
                                                        length);
  gchar *https_request_uri_string = g_strconcat (priv->proxy_server_uri,
                                                 checksum_string, NULL);
  g_free (checksum_string);

  SoupURI *https_request_uri = soup_uri_new (https_request_uri_string);

  if (https_request_uri == NULL)
    g_error ("Invalid URI: %s.", https_request_uri_string);

  g_free (https_request_uri_string);

  return https_request_uri;
}

/*
 * Sets an absolute timestamp and a boot-offset-corrected relative timestamp in
 * the out parameters. Returns FALSE on failure and TRUE on success. The values
 * of the out parameters are undefined on failure.
 */
static gboolean
get_offset_timestamps (EmerDaemon *self,
                       gint64     *rel_timestamp_ptr,
                       gint64     *abs_timestamp_ptr)
{

  if (!get_current_time (CLOCK_BOOTTIME, rel_timestamp_ptr) ||
      !get_current_time (CLOCK_REALTIME, abs_timestamp_ptr))
    return FALSE;

  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GError *error = NULL;
  gint64 boot_offset;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, &error, FALSE))
    {
      g_warning ("Persistent cache could not get boot offset: %s.",
                 error->message);
      g_error_free (error);
      return FALSE;
    }

  *rel_timestamp_ptr += boot_offset;
  return TRUE;
}

static GVariant *
get_updated_request_body (EmerDaemon *self,
                          GVariant   *request_body)
{
  gint32 client_version_number;
  GVariantIter *machine_id_iter;
  GVariantIter *singulars_iter, *aggregates_iter, *sequences_iter;
  g_variant_get (request_body, "(ixxaya(uayxmv)a(uayxxmv)a(uaya(xmv)))",
                 &client_version_number, NULL, NULL, &machine_id_iter,
                 &singulars_iter, &aggregates_iter, &sequences_iter);

  GVariantBuilder machine_id_builder;
  get_builder_from_iter (machine_id_iter, &machine_id_builder,
                         G_VARIANT_TYPE ("ay"));
  g_variant_iter_free (machine_id_iter);

  GVariantBuilder singulars_builder;
  get_builder_from_iter (singulars_iter, &singulars_builder,
                         G_VARIANT_TYPE ("a(uayxmv)"));
  g_variant_iter_free (singulars_iter);

  GVariantBuilder aggregates_builder;
  get_builder_from_iter (aggregates_iter, &aggregates_builder,
                         G_VARIANT_TYPE ("a(uayxxmv)"));
  g_variant_iter_free (aggregates_iter);

  GVariantBuilder sequences_builder;
  get_builder_from_iter (sequences_iter, &sequences_builder,
                         G_VARIANT_TYPE ("a(uaya(xmv))"));
  g_variant_iter_free (sequences_iter);

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_offset_timestamps (self, &relative_timestamp, &absolute_timestamp))
    {
      g_warning ("Could not get, or correct, network request timestamps.");
      return NULL;
    }

  gint64 little_endian_relative_timestamp =
    swap_bytes_64_if_big_endian (relative_timestamp);
  gint64 little_endian_absolute_timestamp =
    swap_bytes_64_if_big_endian (absolute_timestamp);

  GVariant *updated_request_body =
    g_variant_new ("(ixxaya(uayxmv)a(uayxxmv)a(uaya(xmv)))",
                   client_version_number, little_endian_relative_timestamp,
                   little_endian_absolute_timestamp, &machine_id_builder,
                   &singulars_builder, &aggregates_builder, &sequences_builder);

  return updated_request_body;
}

// Handles HTTP or HTTPS responses.
static void
handle_https_response (SoupSession         *https_session,
                       SoupMessage         *https_message,
                       NetworkCallbackData *callback_data)
{
  guint status_code;
  g_object_get (https_message, "status-code", &status_code, NULL);
  if (SOUP_STATUS_IS_SUCCESSFUL (status_code))
    {
      free_network_callback_data (callback_data);
      return;
    }

  gchar *reason_phrase;
  g_object_get (https_message, "reason-phrase", &reason_phrase, NULL);
  g_warning ("Attempt to upload metrics failed: %s.", reason_phrase);
  g_free (reason_phrase);

  EmerDaemon *self = callback_data->daemon;
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (++callback_data->attempt_num >= NETWORK_ATTEMPT_LIMIT)
    {
      g_debug ("Maximum number of network attempts (%d) reached -- dropping "
               "metrics.", NETWORK_ATTEMPT_LIMIT);
      free_network_callback_data (callback_data);
      return;
    }

  if (SOUP_STATUS_IS_TRANSPORT_ERROR (status_code) ||
      SOUP_STATUS_IS_CLIENT_ERROR (status_code) ||
      SOUP_STATUS_IS_SERVER_ERROR (status_code))
    {
      backoff (priv->rand, callback_data->attempt_num);
      GVariant *updated_request_body =
        get_updated_request_body (self, callback_data->request_body);

      if (updated_request_body == NULL)
        {
          g_warning ("Could not update network request with current "
                     "timestamps. Dropping metrics.");
          free_network_callback_data (callback_data);
          return;
        }

      g_variant_unref (callback_data->request_body);
      callback_data->request_body = updated_request_body;

      gconstpointer serialized_request_body =
        g_variant_get_data (updated_request_body);
      if (serialized_request_body == NULL)
        {
          g_warning ("Could not serialize updated network request body. "
                     "Dropping metrics.");
          free_network_callback_data (callback_data);
          return;
        }

      gsize request_body_length = g_variant_get_size (updated_request_body);

      SoupURI *https_request_uri =
        get_https_request_uri (callback_data->daemon, serialized_request_body,
                               request_body_length);
      SoupMessage *new_https_message =
        soup_message_new_from_uri ("PUT",  https_request_uri);
      soup_uri_free (https_request_uri);

      soup_message_set_request (new_https_message, "application/octet-stream",
                                SOUP_MEMORY_TEMPORARY, serialized_request_body,
                                request_body_length);

      soup_session_queue_message (https_session, new_https_message,
                                  (SoupSessionCallback) handle_https_response,
                                  callback_data);
      /* Old message is unreffed automatically, because it is not requeued. */

      return;
    }

  g_warning ("Received HTTP status code: %u. Dropping metrics.", status_code);
  free_network_callback_data (callback_data);
}

static void
drain_singulars_buffer (EmerDaemonPrivate *priv,
                        GVariantBuilder   *singulars_builder)
{
  g_mutex_lock (&priv->singular_buffer_lock);

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

  g_mutex_unlock (&priv->singular_buffer_lock);
}

static void
drain_aggregates_buffer (EmerDaemonPrivate *priv,
                         GVariantBuilder   *aggregates_builder)
{
  g_mutex_lock (&priv->aggregate_buffer_lock);

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

  g_mutex_unlock (&priv->aggregate_buffer_lock);
}

static void
drain_sequences_buffer (EmerDaemonPrivate *priv,
                        GVariantBuilder   *sequences_builder)
{
  g_mutex_lock (&priv->sequence_buffer_lock);

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

  g_mutex_unlock (&priv->sequence_buffer_lock);
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

  /* TODO: This value is currently unused by the persistent cache, but should be
     updated to a sensible value when the PC starts using it. */
  gint maximum_bytes_to_drain = 92160;
  if (!emer_persistent_cache_drain_metrics (priv->persistent_cache,
                                            &list_of_singulars,
                                            &list_of_aggregates,
                                            &list_of_sequences,
                                            maximum_bytes_to_drain))
    return FALSE;

  for (gint i = 0; list_of_singulars[i] != NULL; i++)
    {
      g_variant_builder_add_value (singulars_builder, list_of_singulars[i]);
      g_variant_unref (list_of_singulars[i]);
    }
  g_free (list_of_singulars);

  for (gint i = 0; list_of_aggregates[i] != NULL; i++)
    {
      g_variant_builder_add_value (aggregates_builder, list_of_aggregates[i]);
      g_variant_unref (list_of_aggregates[i]);
    }
  g_free (list_of_aggregates);

  for (gint i = 0; list_of_sequences[i] != NULL; i++)
    {
      g_variant_builder_add_value (sequences_builder, list_of_sequences[i]);
      g_variant_unref (list_of_sequences[i]);
    }
  g_free (list_of_sequences);

  return TRUE;
}

static GVariant *
create_request_body (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  uuid_t machine_id;
  gboolean read_id = emer_machine_id_provider_get_id (priv->machine_id_provider,
                                                      machine_id);
  if (!read_id)
    return NULL;

  GVariantBuilder machine_id_builder;
  get_uuid_builder (machine_id, &machine_id_builder);

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
  if (!get_offset_timestamps (self, &relative_timestamp, &absolute_timestamp))
    {
      g_warning ("Could not get, or correct, network request timestamps.");
      return NULL;
    }

  GVariant *request_body =
    g_variant_new ("(ixxaya(uayxmv)a(uayxxmv)a(uaya(xmv)))",
                   priv->client_version_number, relative_timestamp,
                   absolute_timestamp, &machine_id_builder, &singulars_builder,
                   &aggregates_builder, &sequences_builder);

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
 * itself. The calling code is expected to be holding the lock on the singular
 * buffer.
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

  g_mutex_lock (&priv->singular_buffer_lock);
  g_mutex_lock (&priv->aggregate_buffer_lock);
  g_mutex_lock (&priv->sequence_buffer_lock);

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
  g_mutex_unlock (&priv->singular_buffer_lock);

  trash_stored_aggregates (self, num_aggregates_stored);
  g_mutex_unlock (&priv->aggregate_buffer_lock);

  trash_stored_sequences (self, num_sequences_stored);
  g_mutex_unlock (&priv->sequence_buffer_lock);
}

static void
upload_events (EmerDaemon   *self,
               GAsyncResult *res)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  GError *error = NULL;
  if (!g_network_monitor_can_reach_finish (priv->network_monitor, res, &error))
    {
      g_debug ("Network Monitor says we can't reach the network. Sending "
               "metrics to persistent cache. Error: %s.", error->message);
      g_error_free (error);
      flush_to_persistent_cache (self);
      return;
    }

  GVariant *request_body = create_request_body (self);
  if (request_body == NULL)
  {
    g_object_unref (self);
    return;
  }

  gconstpointer serialized_request_body = g_variant_get_data (request_body);
  if (serialized_request_body == NULL)
    {
      g_warning ("Could not serialize network request body.");
      g_object_unref (self);
      return;
    }

  gsize request_body_length = g_variant_get_size (request_body);

  SoupURI *https_request_uri =
    get_https_request_uri (self, serialized_request_body, request_body_length);
  SoupMessage *https_message =
    soup_message_new_from_uri ("PUT",  https_request_uri);
  soup_uri_free (https_request_uri);

  soup_message_set_request (https_message, "application/octet-stream",
                            SOUP_MEMORY_TEMPORARY, serialized_request_body,
                            request_body_length);

  NetworkCallbackData *callback_data = g_new (NetworkCallbackData, 1);
  callback_data->daemon = self;
  callback_data->request_body = request_body;
  callback_data->attempt_num = 0;
  soup_session_queue_message (priv->http_session, https_message,
                              (SoupSessionCallback) handle_https_response,
                              callback_data);

  g_object_unref (self);
}

static gboolean
check_and_upload_events (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  /* Decrease the chances of a race with emer_daemon_finalize. */
  g_object_ref (self);

  g_network_monitor_can_reach_async (priv->network_monitor,
                                     priv->ping_socket,
                                     NULL,
                                     (GAsyncReadyCallback) upload_events,
                                     NULL);
  return G_SOURCE_CONTINUE;
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
      return;
    }

  priv->shutdown_inhibitor = fds[0];
  g_free (fds);
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
      gboolean before_shutdown;
      g_variant_get_child (parameters, 0, "b", &before_shutdown);
      if (before_shutdown)
        {
          flush_to_persistent_cache (self);
          release_shutdown_inhibitor (self);
        }
      else
        {
          inhibit_shutdown (self);
        }
    }
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
 * The following functions are private setters and getters for the properties of
 * EmerDaemon. These properties are write-only, construct-only, so these only
 * need to be internal.
 */

static void
set_random_number_generator (EmerDaemon *self,
                             GRand      *rand)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->rand = rand == NULL ? g_rand_new () : rand;
}

static GRand *
get_random_number_generator (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->rand;
}

static void
set_client_version_number (EmerDaemon *self,
                           gint        number)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->client_version_number = number;
}

static gint
get_client_version_number (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->client_version_number;
}

static void
set_network_send_interval (EmerDaemon *self,
                           guint       seconds)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->network_send_interval_seconds = seconds;
  priv->upload_events_timeout_source_id =
    g_timeout_add_seconds (seconds, (GSourceFunc) check_and_upload_events,
                           self);
}

static guint
get_network_send_interval (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->network_send_interval_seconds;
}

static void
set_proxy_server_uri (EmerDaemon  *self,
                      const gchar *uri)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->proxy_server_uri = g_strdup (uri);
}

static gchar *
get_proxy_server_uri (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->proxy_server_uri;
}

static void
set_machine_id_provider (EmerDaemon            *self,
                         EmerMachineIdProvider *machine_id_prov)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  if (machine_id_prov == NULL)
    machine_id_prov = emer_machine_id_provider_get_default ();

  priv->machine_id_provider = g_object_ref (machine_id_prov);
}

static EmerMachineIdProvider *
get_machine_id_provider (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->machine_id_provider;
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
          priv->persistent_cache = NULL;
          return;
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
  priv->num_singulars_buffered = 0;
  g_mutex_init (&priv->singular_buffer_lock);
}

static gint
get_singular_buffer_length (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->singular_buffer_length;
}

static void
set_aggregate_buffer_length (EmerDaemon *self,
                             gint        length)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->aggregate_buffer_length = length;
  priv->aggregate_buffer = g_new (AggregateEvent, length);
  priv->num_aggregates_buffered = 0;
  g_mutex_init (&priv->aggregate_buffer_lock);
}

static gint
get_aggregate_buffer_length (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->aggregate_buffer_length;
}

static void
set_sequence_buffer_length (EmerDaemon *self,
                            gint        length)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->sequence_buffer_length = length;
  priv->sequence_buffer = g_new (SequenceEvent, length);
  priv->num_sequences_buffered = 0;
  g_mutex_init (&priv->sequence_buffer_lock);

}

static gint
get_sequence_buffer_length (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->sequence_buffer_length;
}

static void
emer_daemon_constructed (GObject *object)
{
  EmerDaemon *self = EMER_DAEMON (object);
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  priv->recording_enabled =
    emer_permissions_provider_get_daemon_enabled (priv->permissions_provider);

  GError *error = NULL;
  priv->ping_socket = g_network_address_parse_uri (priv->proxy_server_uri,
                                                   443, // SSL default port
                                                   &error);
  if (priv->ping_socket == NULL)
    g_error ("Invalid proxy server URI '%s' could not be parsed because: %s.",
             priv->proxy_server_uri, error->message);

  G_OBJECT_CLASS (emer_daemon_parent_class)->constructed (object);
}

static void
emer_daemon_get_property (GObject    *object,
                          guint       property_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  EmerDaemon *self = EMER_DAEMON (object);

  switch (property_id)
    {
    case PROP_RANDOM_NUMBER_GENERATOR:
      g_value_set_pointer (value, get_random_number_generator (self));
      break;

    case PROP_CLIENT_VERSION_NUMBER:
      g_value_set_int (value, get_client_version_number (self));
      break;

    case PROP_NETWORK_SEND_INTERVAL:
      g_value_set_uint (value, get_network_send_interval (self));
      break;

    case PROP_PROXY_SERVER_URI:
      g_value_set_string (value, get_proxy_server_uri (self));
      break;

    case PROP_MACHINE_ID_PROVIDER:
      g_value_set_object (value, get_machine_id_provider (self));
      break;

    case PROP_SINGULAR_BUFFER_LENGTH:
      g_value_set_int (value, get_singular_buffer_length (self));
      break;

    case PROP_AGGREGATE_BUFFER_LENGTH:
      g_value_set_int (value, get_aggregate_buffer_length (self));
      break;

    case PROP_SEQUENCE_BUFFER_LENGTH:
      g_value_set_int (value, get_sequence_buffer_length (self));
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

    case PROP_CLIENT_VERSION_NUMBER:
      set_client_version_number (self, g_value_get_int (value));
      break;

    case PROP_NETWORK_SEND_INTERVAL:
      set_network_send_interval (self, g_value_get_uint (value));
      break;

    case PROP_PROXY_SERVER_URI:
      set_proxy_server_uri (self, g_value_get_string (value));
      break;

    case PROP_MACHINE_ID_PROVIDER:
      set_machine_id_provider (self, g_value_get_object (value));
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
  release_shutdown_inhibitor (self);
  g_clear_object (&priv->login_manager_proxy);

  g_rand_free (priv->rand);
  g_free (priv->proxy_server_uri);
  g_clear_object (&priv->machine_id_provider);
  g_clear_object (&priv->permissions_provider);
  g_clear_object (&priv->persistent_cache);
  g_clear_object (&priv->ping_socket);
  // Do not free the GNetworkMonitor.  It is transfer none.

  free_singular_buffer (priv->singular_buffer, priv->num_singulars_buffered);
  g_mutex_clear (&priv->singular_buffer_lock);

  free_aggregate_buffer (priv->aggregate_buffer, priv->num_aggregates_buffered);
  g_mutex_clear (&priv->aggregate_buffer_lock);

  free_sequence_buffer (priv->sequence_buffer, priv->num_sequences_buffered);
  g_mutex_clear (&priv->sequence_buffer_lock);

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
   * EmerDaemon:client-version-number:
   *
   * Network protocol version of the metrics client.
   */
  emer_daemon_props[PROP_CLIENT_VERSION_NUMBER] =
    g_param_spec_int ("client-version-number", "Client version number",
                      "Client network protocol version",
                      -1, G_MAXINT, 0,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                      G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_PROXY_SERVER_URI] =
    g_param_spec_string ("proxy-server-uri", "Proxy server URI",
                         "The URI to send metrics to",
                         PRODUCTION_SERVER_URI,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:machine-id-provider:
   *
   * An #EmerMachineIdProvider for retrieving the UUID of this machine.
   * If this property is not specified, the default machine ID provider (from
   * emer_machine_id_provider_get_default()) will be used.
   * You should only set this property to something else for testing purposes.
   */
  emer_daemon_props[PROP_MACHINE_ID_PROVIDER] =
    g_param_spec_object ("machine-id-provider", "Machine ID provider",
                         "Object providing machine ID",
                         EMER_TYPE_MACHINE_ID_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:permissions-provider:
   *
   */
  emer_daemon_props[PROP_PERMISSIONS_PROVIDER] =
    g_param_spec_object ("permissions-provider", "Permissions provider",
                         "Object providing user's permission to record metrics",
                         EMER_TYPE_PERMISSIONS_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:persistent-cache:
   *
   * An #EmerPersistentCache for storing metrics until they are sent to the
   * proxy server.
   * If this property is not specified, a default persistent cache (created by
   * emer_persistent_cache_new ()) will be used.
   * You should only set this property to something else for testing purposes.
   */
  emer_daemon_props[PROP_PERSISTENT_CACHE] =
    g_param_spec_object ("persistent-cache", "Persistent cache",
                         "Object managing persistent storage of metrics until "
                         "they are sent to the proxy server",
                         EMER_TYPE_PERSISTENT_CACHE,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
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
                       0, G_MAXUINT, (60 * 60),
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
}

static void
emer_daemon_init (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  // We are not currently holding a shutdown inhibitor.
  priv->shutdown_inhibitor = -1;

  GError *error = NULL;
  priv->login_manager_proxy =
    g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                   G_DBUS_PROXY_FLAGS_NONE,
                                   NULL /* GDBusInterfaceInfo */,
                                   "org.freedesktop.login1",
                                   "/org/freedesktop/login1",
                                   "org.freedesktop.login1.Manager",
                                   NULL /* GCancellable */, &error);
  if (priv->login_manager_proxy == NULL)
    {
      g_warning ("Error creating login manager D-Bus proxy: %s.",
                 error->message);
      g_error_free (error);
    }
  else
    {
      g_assert (g_signal_connect (priv->login_manager_proxy, "g-signal",
                                  G_CALLBACK (handle_login_manager_signal),
                                  self) > 0);
      inhibit_shutdown (self);
    }

  gchar *user_agent = get_user_agent ();

  priv->http_session =
    soup_session_new_with_options (SOUP_SESSION_MAX_CONNS, 1,
                                   SOUP_SESSION_MAX_CONNS_PER_HOST, 1,
                                   SOUP_SESSION_USER_AGENT, user_agent,
                                   SOUP_SESSION_ADD_FEATURE_BY_TYPE,
                                   SOUP_TYPE_CACHE,
                                   NULL);
  g_free (user_agent);
  priv->network_monitor = g_network_monitor_get_default ();
}

/*
 * emer_daemon_new:
 * @environment: dev/test/production
 *
 * Creates a new EOS Metrics Daemon.
 *
 * Returns: (transfer full): a new #EmerDaemon.
 */
EmerDaemon *
emer_daemon_new (const gchar *environment)
{
  gchar *proxy_server_uri = g_strconcat ("https://", environment, ".metrics.endlessm.com/", NULL);
  return g_object_new (EMER_TYPE_DAEMON,
                       "proxy-server-uri", proxy_server_uri,
                       NULL);
}

/*
 * emer_daemon_new_full:
 * @rand: (allow-none): random number generator to use for randomized
 *   exponential backoff, or %NULL to use the default
 * @version_number: client version of the network protocol
 * @network_send_interval: frequency with which the client will attempt a
 *   network send request
 * @proxy_server_uri: URI to use
 * @machine_id_provider: (allow-none): The #EmerMachineIdProvider to supply the
 *   machine ID, or %NULL to use the default
 * @permissions_provider: The #EmerPermissionsProvider to supply information
 *   about opting out of metrics collection
 * @persistent_cache: (allow-none): The #EmerPersistentCache in which to store
 *   metrics locally when they can't be sent over the network, or %NULL to use
 *   the default.
 * @buffer_length: The maximum size of the buffers to be used for in-memory
 *   storage of metrics
 *
 * Testing function for creating a new EOS Metrics daemon.
 * You should only need to use this for unit testing.
 *
 * Never pass a production metrics proxy server URI for @proxy_server_uri!
 *
 * Returns: (transfer full): a new #EmerDaemon.
 */
EmerDaemon *
emer_daemon_new_full (GRand                   *rand,
                      gint                     version_number,
                      guint                    network_send_interval,
                      const gchar             *proxy_server_uri,
                      EmerMachineIdProvider   *machine_id_provider,
                      EmerPermissionsProvider *permissions_provider,
                      EmerPersistentCache     *persistent_cache,
                      gint                     buffer_length)
{
  return g_object_new (EMER_TYPE_DAEMON,
                       "random-number-generator", rand,
                       "client-version-number", version_number,
                       "network-send-interval", network_send_interval,
                       "proxy-server-uri", proxy_server_uri,
                       "machine-id-provider", machine_id_provider,
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

  gint64 boot_offset;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, NULL, FALSE))
    {
      g_warning ("Unable to correct metric's relative timestamp. "
                 "Dropping metric.");
      return;
    }
  relative_timestamp += boot_offset;

  g_mutex_lock (&priv->singular_buffer_lock);

  if (priv->num_singulars_buffered < priv->singular_buffer_length)
    {
      SingularEvent *singular = priv->singular_buffer +
        priv->num_singulars_buffered;
      priv->num_singulars_buffered++;
      singular->user_id = user_id;
      uuid_from_gvariant (event_id, singular->event_id);
      GVariant *nullable_payload = get_nullable_payload (has_payload, payload);
      EventValue event_value = { relative_timestamp, nullable_payload };
      singular->event_value = event_value;
    }

  g_mutex_unlock (&priv->singular_buffer_lock);
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

  gint64 boot_offset;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, NULL, FALSE))
    {
      g_warning ("Unable to correct metric's relative timestamp. "
                 "Dropping metric.");
      return;
    }
  relative_timestamp += boot_offset;

  g_mutex_lock (&priv->aggregate_buffer_lock);

  if (priv->num_aggregates_buffered < priv->aggregate_buffer_length)
    {
      AggregateEvent *aggregate = priv->aggregate_buffer +
        priv->num_aggregates_buffered;
      priv->num_aggregates_buffered++;
      SingularEvent singular;
      singular.user_id = user_id;
      uuid_from_gvariant (event_id, singular.event_id);
      aggregate->num_events = num_events;
      GVariant *nullable_payload = get_nullable_payload (has_payload, payload);
      EventValue event_value = { relative_timestamp, nullable_payload };
      singular.event_value = event_value;
      aggregate->event = singular;
    }

  g_mutex_unlock (&priv->aggregate_buffer_lock);
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

  gint64 boot_offset;
  if (!emer_persistent_cache_get_boot_time_offset (priv->persistent_cache,
                                                   &boot_offset, NULL, FALSE))
    {
      g_warning ("Unable to correct metric's relative timestamp. "
                 "Dropping metric.");
      return;
    }

  g_mutex_lock (&priv->sequence_buffer_lock);

  if (priv->num_sequences_buffered < priv->sequence_buffer_length)
    {
      SequenceEvent *event_sequence =
        priv->sequence_buffer + priv->num_sequences_buffered;
      priv->num_sequences_buffered++;

      event_sequence->user_id = user_id;
      uuid_from_gvariant (event_id, event_sequence->event_id);

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

  g_mutex_unlock (&priv->sequence_buffer_lock);
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
