/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#define _POSIX_C_SOURCE 200112L

#include "emtr-event-recorder.h"
#include "emer-event-recorder-server.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <uuid/uuid.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>

/* Convenience macro to check that @ptr is a #GVariant */
#define _IS_VARIANT(ptr) (g_variant_is_of_type ((ptr), G_VARIANT_TYPE_ANY))

// The number of nanoseconds in one second.
#define NANOSECONDS_PER_SECOND 1000000000L

// TODO: Once we have a production proxy server, update this constant
// accordingly.
// The URI of the metrics production proxy server.
// Is kept in a #define to prevent testing code from sending metrics to the
// production server.
#define PRODUCTION_SERVER_URI "http://metrics-test.endlessm-sf.com:8080/"

/*
 * The number of elements in a uuid_t. uuid_t is assumed to be a fixed-length
 * array of guchar.
 */
#define UUID_LENGTH (sizeof (uuid_t) / sizeof (guchar))

/**
 * SECTION:emtr-event-recorder
 * @title: Event Recorder
 * @short_description: Records metric events to metric system daemon.
 * @include: eosmetrics/eosmetrics.h
 *
 * The event recorder asynchronously sends metric events to the metric system
 * daemon via D-Bus. The system daemon then delivers metrics to the server on
 * a best-effort basis. No feedback is given regarding the outcome of delivery.
 * The event recorder is thread-safe.
 *
 * This API may be called from Javascript as follows.
 *
 * |[
 * const EosMetrics = imports.gi.EosMetrics;
 * const GLib = imports.gi.GLib;
 * const MEANINGLESS_EVENT = "fb59199e-5384-472e-af1e-00b7a419d5c2";
 * const MEANINGLESS_AGGREGATED_EVENT = "01ddd9ad-255a-413d-8c8c-9495d810a90f";
 * const MEANINGLESS_EVENT_WITH_AUX_DATA =
 *   "9f26029e-8085-42a7-903e-10fcd1815e03";
 *
 * let eventRecorder = EosMetrics.EventRecorder.get_default();
 *
 * // Records a single instance of MEANINGLESS_EVENT along with the current
 * // time.
 * eventRecorder.record_event(MEANINGLESS_EVENT, null);
 *
 * // Records the fact that MEANINGLESS_AGGREGATED_EVENT occurred 23
 * // times since the last time it was recorded.
 * eventRecorder.record_events(MEANINGLESS_AGGREGATED_EVENT,
 *   23, null);
 *
 * // Records MEANINGLESS_EVENT_WITH_AUX_DATA along with some auxiliary data and
 * // the current time.
 * eventRecorder.record_event(MEANINGLESS_EVENT_WITH_AUX_DATA,
 *   new GLib.Variant('a{sv}', {
 *     units_of_smoke_ground: new GLib.Variant('u', units),
 *     grinding_time: new GLib.Variant('u', time)
 *   }););
 * ]|
 */

typedef struct EventValue
{
  // Time elapsed in nanoseconds from an unspecified starting point.
  gint64 relative_time;

  GVariant *auxiliary_payload;
} EventValue;

typedef struct Event
{
  uuid_t event_id;
  EventValue event_value;
} Event;

typedef struct Aggregate
{
  Event event;
  gint64 num_events;
} Aggregate;

typedef struct EventSequence
{
  uuid_t event_id;
  GVariant *key;

  /*
   * The first element is the start event, the last element is the stop event,
   * and any elements in between are progress events. The elements are ordered
   * chronologically.
   */
  EventValue *event_values;

  gint num_event_values;
} EventSequence;

typedef struct EmtrEventRecorderPrivate
{
  gint client_version;

  gchar *environment;

  guint network_send_interval_seconds;

  gint individual_buffer_length;
  gint aggregate_buffer_length;
  gint sequence_buffer_length;

  gchar *proxy_server_uri;

  EmtrMachineIdProvider *machine_id_provider;

  Event * volatile event_buffer;
  volatile gint num_events_buffered;
  GMutex event_buffer_lock;

  Aggregate * volatile aggregate_buffer;
  volatile gint num_aggregates_buffered;
  GMutex aggregate_buffer_lock;

  EventSequence * volatile event_sequence_buffer;
  volatile gint num_event_sequences_buffered;
  GMutex event_sequence_buffer_lock;

  GHashTable * volatile events_by_id_with_key;
  GMutex events_by_id_with_key_lock;

  SoupSession *http_session;

  /*
   * DBus doesn't support maybe types, so a boolean is used to indicate whether
   * the auxiliary_payload field should be ignored. A non-NULL auxiliary_payload
   * must be passed even when it will be ignored, and this is the arbitrary
   * variant that is used for that purpose.
   */
  GVariant *empty_auxiliary_payload;

  EmerEventRecorderServer *dbus_proxy;

  gboolean recording_enabled;

  guint upload_events_timeout_source_id;
} EmtrEventRecorderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmtrEventRecorder, emtr_event_recorder, G_TYPE_OBJECT)

static void
free_event (Event *event)
{
  GVariant *auxiliary_payload = event->event_value.auxiliary_payload;
  if (G_UNLIKELY (auxiliary_payload != NULL))
    g_variant_unref (auxiliary_payload);
}

static void
free_event_sequence (EventSequence *event_sequence)
{
  g_variant_unref (event_sequence->key);

  for (gint i = 0; i < event_sequence->num_event_values; ++i)
    {
       GVariant *curr_auxiliary_payload =
         event_sequence->event_values[i].auxiliary_payload;
       if (G_UNLIKELY (curr_auxiliary_payload != NULL))
         g_variant_unref (curr_auxiliary_payload);
    }

  g_free (event_sequence->event_values);
}

static void
emtr_event_recorder_finalize (GObject *object)
{
  EmtrEventRecorder *self = EMTR_EVENT_RECORDER (object);
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  g_source_remove (priv->upload_events_timeout_source_id);

  g_free (priv->environment);
  g_free (priv->proxy_server_uri);
  g_object_unref (priv->machine_id_provider);

  Event *event_buffer = priv->event_buffer;
  gint num_events = priv->num_events_buffered;
  for (gint i = 0; i < num_events; ++i)
    free_event (event_buffer + i);

  g_free (event_buffer);
  g_mutex_clear (&(priv->event_buffer_lock));

  Aggregate *aggregate_buffer = priv->aggregate_buffer;
  gint num_aggregates = priv->num_aggregates_buffered;
  for (gint i = 0; i < num_aggregates; ++i)
    free_event (&(aggregate_buffer[i].event));

  g_free (aggregate_buffer);
  g_mutex_clear (&(priv->aggregate_buffer_lock));

  EventSequence *event_sequence_buffer = priv->event_sequence_buffer;
  gint num_event_sequences = priv->num_event_sequences_buffered;
  for (gint i = 0; i < num_event_sequences; ++i)
    free_event_sequence (event_sequence_buffer + i);

  g_free (event_sequence_buffer);
  g_mutex_clear (&(priv->event_sequence_buffer_lock));

  g_hash_table_destroy (priv->events_by_id_with_key);
  g_mutex_clear (&(priv->events_by_id_with_key_lock));

  g_object_unref (priv->http_session);

  g_variant_unref (priv->empty_auxiliary_payload);

  if (priv->dbus_proxy != NULL)
    g_object_unref (priv->dbus_proxy);

  G_OBJECT_CLASS (emtr_event_recorder_parent_class)->finalize (object);
}



/*
 * https://developer.gnome.org/glib/2.40/glib-GVariant.html#g-variant-hash
 * does not work on container types, so we implement our own more general, hash
 * function. Note that the GVariant is trusted to be in fully-normalized form.
 * The implementation is inspired by the GLib implementations of g_str_hash and
 * g_bytes_hash.
 */
static guint
general_variant_hash (gconstpointer key)
{
  GVariant *variant = (GVariant *) key;
  const gchar *type_string = g_variant_get_type_string (variant);
  guint hash_value = g_str_hash (type_string);
  GBytes *serialized_data = g_variant_get_data_as_bytes (variant);
  if (G_LIKELY (serialized_data != NULL))
    {
      hash_value = (hash_value * 33) + g_bytes_hash (serialized_data);
      g_bytes_unref (serialized_data);
    }
  return hash_value;
}

// Handles HTTP or HTTPS responses.
static void
handle_https_response (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  SoupRequest *https_request = (SoupRequest *) source_object;
  GError *error = NULL;
  GInputStream *response_stream = soup_request_send_finish (https_request,
                                                            result, &error);

  GVariant *request_body = (GVariant *) user_data;
  g_variant_unref (request_body);

  if (G_UNLIKELY (response_stream == NULL))
    {
      g_warning ("Error receiving metric HTTPS response: %s", error->message);
      g_error_free (error);
      return;
    }

  // TODO: Read and react to response.
  g_object_unref (response_stream);
}

static void
get_uuid_builder (uuid_t uuid, GVariantBuilder *uuid_builder)
{
  g_variant_builder_init (uuid_builder, G_VARIANT_TYPE ("ay"));
  for (size_t i = 0; i < UUID_LENGTH; ++i)
    g_variant_builder_add (uuid_builder, "y", uuid[i]);
}

static gboolean
get_current_time (clockid_t clock_id, gint64 *current_time)
{
  g_assert (current_time != NULL);

  // Get the time before doing anything else because it will change during
  // execution.
  struct timespec ts;
  int gettime_failed = clock_gettime (clock_id, &ts);
  if (G_UNLIKELY (gettime_failed != 0))
    {
      int error_code = errno;
      g_critical ("Attempt to get current time failed with error code: %d.",
                  error_code);
      return FALSE;
    }

  // Ensure that the clock provides a time that can be safely represented in a
  // gint64 in nanoseconds.
  g_assert (ts.tv_sec >= (G_MININT64 / NANOSECONDS_PER_SECOND));
  g_assert (ts.tv_sec <= (G_MAXINT64 / NANOSECONDS_PER_SECOND));
  g_assert (ts.tv_nsec >= 0);
  g_assert (ts.tv_nsec < NANOSECONDS_PER_SECOND);

  // We already know that ts.tv_sec <= (G_MAXINT64 / NANOSECONDS_PER_SECOND).
  // This handles the edge case where
  // ts.tv_sec == (G_MAXINT64 / NANOSECONDS_PER_SECOND).
  g_assert ((ts.tv_sec < (G_MAXINT64 / NANOSECONDS_PER_SECOND)) ||
            (ts.tv_nsec <= (G_MAXINT64 % NANOSECONDS_PER_SECOND)));

  *current_time = (NANOSECONDS_PER_SECOND * ((gint64) ts.tv_sec))
    + ((gint64) ts.tv_nsec);
  return TRUE;
}

static void
get_system_events_builder (EmtrEventRecorderPrivate *priv,
                           GVariantBuilder          *system_events_builder)
{
  g_variant_builder_init (system_events_builder, G_VARIANT_TYPE ("a(ayxmv)"));

  g_mutex_lock (&(priv->event_buffer_lock));

  Event *event_buffer = priv->event_buffer;
  gint num_events = priv->num_events_buffered;
  for (gint i = 0; i < num_events; ++i)
    {
      Event *curr_event = event_buffer + i;
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_event->event_id, &event_id_builder);
      g_variant_builder_add (system_events_builder, "(ayxmv)",
        &event_id_builder, curr_event->event_value.relative_time,
        curr_event->event_value.auxiliary_payload);
      free_event (curr_event);
    }
  priv->num_events_buffered = 0;

  g_mutex_unlock (&(priv->event_buffer_lock));
}

static void
get_system_aggregates_builder (EmtrEventRecorderPrivate *priv,
                               GVariantBuilder          *system_aggregates_builder)
{
  g_variant_builder_init (system_aggregates_builder,
                          G_VARIANT_TYPE ("a(ayxxmv)"));

  g_mutex_lock (&(priv->aggregate_buffer_lock));

  Aggregate *aggregate_buffer = priv->aggregate_buffer;
  gint num_aggregates = priv->num_aggregates_buffered;
  for (gint i = 0; i < num_aggregates; ++i)
    {
      Aggregate *curr_aggregate = aggregate_buffer + i;
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_aggregate->event.event_id, &event_id_builder);
      g_variant_builder_add (system_aggregates_builder, "(ayxxmv)",
        &event_id_builder, curr_aggregate->event.event_value.relative_time,
        curr_aggregate->num_events,
        curr_aggregate->event.event_value.auxiliary_payload);
      free_event (&(curr_aggregate->event));
    }
  priv->num_aggregates_buffered = 0;

  g_mutex_unlock (&(priv->aggregate_buffer_lock));
}

static void
get_system_event_sequences_builder (EmtrEventRecorderPrivate *priv,
                                    GVariantBuilder          *system_event_sequences_builder)
{
  g_variant_builder_init (system_event_sequences_builder,
                          G_VARIANT_TYPE ("a(aya(xmv))"));

  g_mutex_lock (&(priv->event_sequence_buffer_lock));

  EventSequence *event_sequence_buffer = priv->event_sequence_buffer;
  gint num_event_sequences = priv->num_event_sequences_buffered;
  for (gint i = 0; i < num_event_sequences; ++i)
    {
      EventSequence *curr_event_sequence = event_sequence_buffer + i;
      GVariantBuilder event_values_builder;
      g_variant_builder_init (&event_values_builder,
                              G_VARIANT_TYPE ("a(xmv)"));
      for (gint j = 0; j < curr_event_sequence->num_event_values; ++j)
        {
          EventValue *curr_event_value = curr_event_sequence->event_values + j;
          g_variant_builder_add (&event_values_builder, "(xmv)",
                                 curr_event_value->relative_time,
                                 curr_event_value->auxiliary_payload);
        }
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_event_sequence->event_id, &event_id_builder);
      g_variant_builder_add (system_event_sequences_builder, "(aya(xmv))",
        &event_id_builder, &event_values_builder);
      free_event_sequence (curr_event_sequence);
    }
  priv->num_event_sequences_buffered = 0;

  g_mutex_unlock (&(priv->event_sequence_buffer_lock));
}

static GVariant *
create_request_body (EmtrEventRecorderPrivate *priv)
{
  GVariantBuilder machine_id_builder;
  uuid_t machine_id;
  gboolean read_id = emtr_machine_id_provider_get_id (priv->machine_id_provider, machine_id);
  if (!read_id)
    return NULL;
  get_uuid_builder (machine_id, &machine_id_builder);

  GVariantBuilder user_events_builder;
  g_variant_builder_init (&user_events_builder,
                          G_VARIANT_TYPE ("a(aya(ayxmv)a(ayxxmv)a(aya(xmv)))"));
  // TODO: Populate user-specific events. Right now all metrics are considered
  // system-level.

  GVariantBuilder system_events_builder;
  get_system_events_builder (priv, &system_events_builder);

  GVariantBuilder system_aggregates_builder;
  get_system_aggregates_builder (priv, &system_aggregates_builder);

  GVariantBuilder system_event_sequences_builder;
  get_system_event_sequences_builder (priv, &system_event_sequences_builder);

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  gint64 relative_time, absolute_time;
  if (G_UNLIKELY (!get_current_time (CLOCK_BOOTTIME, &relative_time)) ||
      G_UNLIKELY (!get_current_time (CLOCK_REALTIME, &absolute_time)))
    return NULL;

  GVariant *request_body =
    g_variant_new ("(ixxaysa(aya(ayxmv)a(ayxxmv)a(aya(xmv)))"
                   "a(ayxmv)a(ayxxmv)a(aya(xmv)))", priv->client_version,
                   relative_time, absolute_time, &machine_id_builder,
                   priv->environment, &user_events_builder, &system_events_builder,
                   &system_aggregates_builder, &system_event_sequences_builder);

  GVariant *little_endian_request_body = request_body;
  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    {
      little_endian_request_body = g_variant_byteswap (request_body);
      g_variant_unref (request_body);
    }
  else
    {
      g_assert (G_BYTE_ORDER == G_LITTLE_ENDIAN);
    }

  return little_endian_request_body;
}

static gchar *
get_https_request_uri (EmtrEventRecorder *self, const guchar *data, gsize length)
{
  gchar *checksum_string = g_compute_checksum_for_data (G_CHECKSUM_SHA512, data,
                                                        length);
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  gchar *https_request_uri = g_strconcat (priv->proxy_server_uri, checksum_string,
                                          NULL);
  g_free (checksum_string);
  return https_request_uri;
}

static gboolean
upload_events (gpointer user_data)
{
  EmtrEventRecorder *self = (EmtrEventRecorder *) user_data;
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  if (!priv->recording_enabled)
    return G_SOURCE_CONTINUE;

  // Decrease the chances of a race with emtr_event_recorder_finalize.
  g_object_ref (self);

  GVariant *request_body = create_request_body (priv);
  if (request_body == NULL)
    return G_SOURCE_CONTINUE;

  gconstpointer serialized_request_body = g_variant_get_data (request_body);
  g_assert (serialized_request_body != NULL);
  gsize request_body_length = g_variant_get_size (request_body);
  gchar *https_request_uri =
    get_https_request_uri (self, serialized_request_body, request_body_length);

  GError *error = NULL;
  SoupRequestHTTP *https_request =
    soup_session_request_http (priv->http_session, "PUT", https_request_uri,
                               &error);
  g_free (https_request_uri);
  if (G_UNLIKELY (https_request == NULL))
    {
      g_warning ("Error creating metric HTTPS request: %s", error->message);
      g_error_free (error);
      return G_SOURCE_CONTINUE;
    }

  SoupMessage *https_message = soup_request_http_get_message (https_request);
  soup_message_set_request (https_message, "application/octet-stream",
                            SOUP_MEMORY_TEMPORARY, serialized_request_body,
                            request_body_length);
  soup_request_send_async ((SoupRequest *) https_request,
                           NULL /* GCancellable */,
                           (GAsyncReadyCallback) handle_https_response,
                           request_body);

  g_object_unref (https_message);
  g_object_unref (https_request);

  g_object_unref (self);

  return G_SOURCE_CONTINUE;
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

enum {
  PROP_0,
  PROP_NETWORK_SEND_INTERVAL,
  PROP_CLIENT_VERSION_NUMBER,
  PROP_ENVIRONMENT,
  PROP_PROXY_SERVER_URI,
  PROP_INDIVIDUAL_BUFFER_LENGTH,
  PROP_AGGREGATE_BUFFER_LENGTH,
  PROP_SEQUENCE_BUFFER_LENGTH,
  PROP_MACHINE_ID_PROVIDER,
  NPROPS
};

static GParamSpec *emtr_event_recorder_props[NPROPS] = { NULL, };

static void
set_network_send_interval (EmtrEventRecorder *self,
                           guint              seconds)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->network_send_interval_seconds = seconds;
  priv->upload_events_timeout_source_id = g_timeout_add_seconds (seconds,
                                                                 upload_events,
                                                                 self);
}

/*
 * get_network_send_interval:
 * @self: the event recorder
 *
 * See #EmtrEventRecorder:network-send-interval.
 *
 * Returns: the number of seconds between network request
 * send attempts. On invalid input returns 0 instead.
 */
static guint
get_network_send_interval (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), 0);
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->network_send_interval_seconds;
}

static void
set_client_version_number (EmtrEventRecorder *self,
                           gint               number)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->client_version = number;
}

/*
 * get_client_version_number:
 * @self: the event recorder
 *
 * Returns: the client version number. On invalid
 * input, returns -1.
 */
static gint
get_client_version_number (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), -1);
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->client_version;
}

static void
set_environment (EmtrEventRecorder *self,
                 const gchar       *env)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->environment = g_strdup (env);
}

/*
 * get_environment:
 * @self: the event recorder
 *
 * See #EmtrEventRecorder:environment.
 *
 * Returns: the metric user environment. On invalid input,
 * will return the empty string.
 */
static gchar*
get_environment (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), "");
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->environment;
}

static void
set_proxy_server_uri (EmtrEventRecorder *self,
                      const gchar       *uri)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->proxy_server_uri = g_strdup (uri);
}

/*
 * get_proxy_server_uri:
 * @self: the event recorder
 *
 * See #EmtrEventRecorder:proxy-server-uri.
 *
 * Returns: the URI of the proxy server. On invalid input,
 * will return the empty string.
 */
static gchar*
get_proxy_server_uri (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), "");
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->proxy_server_uri;
}

static void
set_individual_buffer_length (EmtrEventRecorder *self,
                              gint               length)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->individual_buffer_length = length;
  priv->event_buffer = g_new (Event, length);
  priv->num_events_buffered = 0;
  g_mutex_init (&(priv->event_buffer_lock));
}

/*
 * get_individual_buffer_length:
 * @self: the event recorder
 *
 * See #EmtrEventRecorder:individual-buffer-length.
 *
 * Returns: the number of metrics that may fit in the individual buffer.
 * On invalid input, returns -1. 
 */
static gint
get_individual_buffer_length (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), -1);
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->individual_buffer_length;
}

static void
set_aggregate_buffer_length (EmtrEventRecorder *self,
                             gint               length)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->aggregate_buffer_length = length;
  priv->aggregate_buffer = g_new (Aggregate, length);
  priv->num_aggregates_buffered = 0;
  g_mutex_init (&(priv->aggregate_buffer_lock));
}

/*
 * get_aggregate_buffer_length:
 * @self: the event recorder
 *
 * See #EmtrEventRecorder:aggregate-buffer-length.
 *
 * Returns: the number of metrics that may fit in the aggregate buffer.
 * On invalid input, returns -1.
 */
static gint
get_aggregate_buffer_length (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), -1);
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->aggregate_buffer_length;
}

static void
set_sequence_buffer_length (EmtrEventRecorder *self,
                            gint               length)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->sequence_buffer_length = length;
  priv->event_sequence_buffer = g_new (EventSequence, length);
  priv->num_event_sequences_buffered = 0;
  g_mutex_init (&(priv->event_sequence_buffer_lock));

}

/*
 * get_sequence_buffer_length:
 * @self: the event recorder
 *
 * See #EmtrEventRecorder:sequence-buffer-length.
 *
 * Returns: the number of metrics that may fit in the sequence buffer.
 * On invalid input, returns -1.
 */
static gint
get_sequence_buffer_length (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), -1);
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->sequence_buffer_length;
}

static void
set_machine_id_provider (EmtrEventRecorder     *self,
                         EmtrMachineIdProvider *machine_id_prov)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);
  priv->machine_id_provider = machine_id_prov;
}

/*
 * get_machine_id_provider
 * @self: the event recorder
 *
 * See #EmtrEventRecorder:machine-id-provider.
 *
 * Returns: the EmtrMachineIdProvider providing a uuid for this object
 */
static EmtrMachineIdProvider *
get_machine_id_provider (EmtrEventRecorder *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self), NULL);
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);
  return priv->machine_id_provider;
}

static void
emtr_event_recorder_set_property (GObject      *object,
                                  guint         property_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  EmtrEventRecorder *self = EMTR_EVENT_RECORDER (object);
  switch (property_id)
    {
    case PROP_NETWORK_SEND_INTERVAL:
      set_network_send_interval (self, g_value_get_uint (value));
      break;

    case PROP_CLIENT_VERSION_NUMBER:
      set_client_version_number (self, g_value_get_int (value));
      break;

    case PROP_ENVIRONMENT:
      set_environment (self, g_value_get_string (value));
      break;

    case PROP_PROXY_SERVER_URI:
      set_proxy_server_uri (self, g_value_get_string (value));
      break;

    case PROP_INDIVIDUAL_BUFFER_LENGTH:
      set_individual_buffer_length (self, g_value_get_int (value));
      break;

    case PROP_AGGREGATE_BUFFER_LENGTH:
      set_aggregate_buffer_length (self, g_value_get_int (value));
      break;

    case PROP_SEQUENCE_BUFFER_LENGTH:
      set_sequence_buffer_length (self, g_value_get_int (value));
      break;

    case PROP_MACHINE_ID_PROVIDER:
      set_machine_id_provider (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emtr_event_recorder_get_property (GObject    *object,
                                  guint       property_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
  EmtrEventRecorder *self = EMTR_EVENT_RECORDER (object);
  switch (property_id)
    {
    case PROP_NETWORK_SEND_INTERVAL:
      g_value_set_uint (value, get_network_send_interval (self));
      break;

    case PROP_CLIENT_VERSION_NUMBER:
      g_value_set_int (value, get_client_version_number (self));
      break;

    case PROP_ENVIRONMENT:
      g_value_set_string (value, get_environment (self));
      break;

    case PROP_PROXY_SERVER_URI:
      g_value_set_string (value, get_proxy_server_uri (self));
      break;

    case PROP_INDIVIDUAL_BUFFER_LENGTH:
      g_value_set_int (value, get_individual_buffer_length (self));
      break;

    case PROP_AGGREGATE_BUFFER_LENGTH:
      g_value_set_int (value, get_aggregate_buffer_length (self));
      break;

    case PROP_SEQUENCE_BUFFER_LENGTH:
      g_value_set_int (value, get_sequence_buffer_length (self));
      break;

    case PROP_MACHINE_ID_PROVIDER:
      g_value_set_object (value, get_machine_id_provider (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emtr_event_recorder_class_init (EmtrEventRecorderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = emtr_event_recorder_get_property;
  object_class->set_property = emtr_event_recorder_set_property;
  object_class->finalize = emtr_event_recorder_finalize;

  /**
   * EmtrEventRecorder:network-send-interval:
   *
   * The frequency with which the client will attempt a network send request, in
   * seconds.
   */
  emtr_event_recorder_props[PROP_NETWORK_SEND_INTERVAL] =
    g_param_spec_uint ("network-send-interval", "Network Send Interval",
                       "Number of seconds until a network request is attempted.",
                       0, G_MAXUINT, (60 * 60), 
                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /**
   * EmtrEventRecorder:client-version-number:
   *
   * Network protocol version of the metrics client.
   */
  emtr_event_recorder_props[PROP_CLIENT_VERSION_NUMBER] =
    g_param_spec_int ("client-version-number", "Client Version Number",
                      "Client network protocol version.",
                      -1, G_MAXINT, 0,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /**
   * EmtrEventRecorder:environment:
   *
   * Specifies what kind of user or system the metrics come from, so that data
   * analysis can exclude metrics from tests or developers' systems.
   *
   * Valid values are "dev", "test", and "prod".
   * The "prod" value, indicating production, should not be used outside of
   * a production system.
   */
  emtr_event_recorder_props[PROP_ENVIRONMENT] =
    g_param_spec_string ("environment", "Environment",
                         "Specifies what kind of user the metrics come from.",
                         "dev",
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emtr_event_recorder_props[PROP_PROXY_SERVER_URI] =
    g_param_spec_string ("proxy-server-uri", "Proxy Server URI",
                         "The URI to send metrics to.",
                         PRODUCTION_SERVER_URI,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emtr_event_recorder_props[PROP_INDIVIDUAL_BUFFER_LENGTH] =
    g_param_spec_int ("individual-buffer-length", "Buffer Length Individual",
                       "The number of metrics allowed to be stored in the individual metric buffer.",
                       -1, G_MAXINT, 2000,
                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emtr_event_recorder_props[PROP_AGGREGATE_BUFFER_LENGTH] =
    g_param_spec_int ("aggregate-buffer-length", "Buffer Length Aggregate",
                      "The number of metrics allowed to be stored in the aggregate metric buffer.",
                      -1, G_MAXINT, 2000,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emtr_event_recorder_props[PROP_SEQUENCE_BUFFER_LENGTH] =
    g_param_spec_int ("sequence-buffer-length", "Buffer Length Sequence",
                      "The number of metrics allowed to be stored in the sequence metric buffer.",
                      -1, G_MAXINT, 2000,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /**
   * EmtrEventRecorder:machine-id-provider:
   *
   * An #EmtrMachineIdProvider for retrieving the unique UUID of this machine.
   * If this property is not specified, the default machine ID provider (from
   * emtr_machine_id_provider_get_default()) will be used.
   * You should only set this property to something else for testing purposes.
   */
  emtr_event_recorder_props[PROP_MACHINE_ID_PROVIDER] = 
    g_param_spec_object ("machine-id-provider", "Machine ID provider",
                         "Object providing unique machine ID",
                         EMTR_TYPE_MACHINE_ID_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emtr_event_recorder_props);
}

static void
emtr_event_recorder_init (EmtrEventRecorder *self)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  priv->events_by_id_with_key =
    g_hash_table_new_full (general_variant_hash, g_variant_equal,
                           (GDestroyNotify) g_variant_unref,
                           (GDestroyNotify) g_array_unref);
  g_mutex_init (&(priv->events_by_id_with_key_lock));

  gchar *user_agent = get_user_agent ();
  priv->http_session =
    soup_session_new_with_options (SOUP_SESSION_MAX_CONNS, 1,
                                   SOUP_SESSION_MAX_CONNS_PER_HOST, 1,
                                   SOUP_SESSION_USER_AGENT, user_agent,
                                   SOUP_SESSION_ADD_FEATURE_BY_TYPE,
                                   SOUP_TYPE_CACHE,
                                   SOUP_SESSION_ADD_FEATURE_BY_TYPE,
                                   SOUP_TYPE_LOGGER,
                                   NULL);
  g_free (user_agent);

  GVariant *unboxed_variant = g_variant_new_boolean (FALSE);
  priv->empty_auxiliary_payload = g_variant_new_variant (unboxed_variant);
  g_variant_ref_sink (priv->empty_auxiliary_payload);


  /* If getting the DBus connection fails, mark self as a no-op object. */
  GError *error = NULL;
  priv->dbus_proxy =
    emer_event_recorder_server_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                       G_DBUS_PROXY_FLAGS_NONE,
                                                       "com.endlessm.Metrics",
                                                       "/com/endlessm/Metrics",
                                                       NULL /* GCancellable */,
                                                       &error);
  if (priv->dbus_proxy == NULL)
    {
      g_critical ("Unable to connect to the DBus event recorder server: %s",
                  error->message);
      g_error_free (error);
      priv->recording_enabled = FALSE;
      return;
    }

  priv->recording_enabled = TRUE;
}

static gboolean
parse_event_id (const gchar *unparsed_event_id,
                uuid_t       parsed_event_id)
{
  int parse_failed = uuid_parse (unparsed_event_id, parsed_event_id);
  if (G_UNLIKELY (parse_failed != 0))
    {
      g_warning ("Attempt to parse UUID \"%s\" failed. Make sure you created "
                 "this UUID with uuidgen -r. You may need to sudo apt-get "
                 "install uuid-runtime first.", unparsed_event_id);
      return FALSE;
    }

  return TRUE;
}

static GVariant *
get_normalized_form_of_variant (GVariant *variant)
{
  GVariant *normalized_variant = NULL;

  if (variant != NULL)
    {
      g_variant_ref_sink (variant);
      normalized_variant = g_variant_get_normal_form (variant);
      g_variant_unref (variant);
    }

  return normalized_variant;
}

static GVariant *
combine_event_id_with_key (uuid_t    event_id,
                           GVariant *key)
{
  GVariantBuilder event_id_builder;
  get_uuid_builder (event_id, &event_id_builder);
  return g_variant_new ("(aymv)", &event_id_builder, key);
}

static void
append_event_value (GArray   *event_values,
                    gint64    relative_time,
                    GVariant *auxiliary_payload)
{
  EventValue prev_event_value = g_array_index (event_values, EventValue,
                                               event_values->len - 1);
  g_assert (relative_time >= prev_event_value.relative_time);
  EventValue curr_event_value = {relative_time, auxiliary_payload};
  g_array_append_val (event_values, curr_event_value);
}

/*
 * Sends the corresponding event_sequence GVariant to D-Bus, which
 * is currently a no-op.
 */
static void
send_event_sequence_to_dbus (EmtrEventRecorderPrivate *priv,
                             GVariant                 *event_id,
                             GArray                   *event_values)
{
  GError *error = NULL;

  // Variants sent to DBus are not allowed to be NULL or maybe types.
  GVariantBuilder event_values_builder;
  g_variant_builder_init (&event_values_builder, G_VARIANT_TYPE ("a(xbv)"));
  for (int i = 0; i < event_values->len; i++)
    {
      EventValue *event_value = &g_array_index (event_values, EventValue, i);
      gboolean has_payload = event_value->auxiliary_payload != NULL;
      GVariant *auxiliary_payload = has_payload ?
        event_value->auxiliary_payload : priv->empty_auxiliary_payload;
      g_variant_builder_add (&event_values_builder, "(xbv)",
                             event_value->relative_time, has_payload,
                             auxiliary_payload);
    }
  GVariant *event_values_variant = g_variant_builder_end (&event_values_builder);
  if (!emer_event_recorder_server_call_record_event_sequence_sync (priv->dbus_proxy,
                                                                   getuid (),
                                                                   event_id,
                                                                   event_values_variant,
                                                                   NULL /* GCancellable */,
                                                                   &error))
    {
      g_warning ("Failed to send event to DBus client-side daemon: %s",
                 error->message);
      g_error_free (error);
    }
}

/*
 * Places an EventSequence struct on the end of the sequence's buffer.
 * Will do nothing if the event_sequence buffer is full.
 */
static void
append_event_sequence_to_buffer (EmtrEventRecorderPrivate *priv,
                                 GVariant                 *event_id_with_key,
                                 GArray                   *event_values)
{
  g_mutex_lock (&(priv->event_sequence_buffer_lock));
  if (priv->num_event_sequences_buffered < priv->sequence_buffer_length)
    {
      EventSequence *event_sequence =
        priv->event_sequence_buffer + priv->num_event_sequences_buffered;
      priv->num_event_sequences_buffered++;
      GVariant *event_id = g_variant_get_child_value (event_id_with_key, 0);
      gsize event_id_length;
      gconstpointer event_id_arr =
        g_variant_get_fixed_array (event_id, &event_id_length, sizeof (guchar));
      g_assert (event_id_length == UUID_LENGTH);
      memcpy (event_sequence->event_id, event_id_arr,
              UUID_LENGTH * sizeof (guchar));
      g_variant_unref (event_id);
      event_sequence->key = g_variant_get_child_value (event_id_with_key, 1);
      event_sequence->event_values = g_new (EventValue, event_values->len);
      memcpy (event_sequence->event_values, event_values->data,
              event_values->len * sizeof (EventValue));
      event_sequence->num_event_values = event_values->len;
    }
  g_mutex_unlock (&(priv->event_sequence_buffer_lock));
}

/* PUBLIC API */

/**
 * emtr_event_recorder_new:
 * @network_send_interval: frequency with which the client will attempt a
 * network send request; see #EmtrEventRecorder:network-send-interval
 * @version_number: client version of the network protocol; see
 * #EmtrEventRecorder:client-version-number
 * @environment: environment of the machine; see #EmtrEventRecorder:environment
 * @proxy_server_uri: URI to use; see #EmtrEventRecorder:proxy-server-uri
 * @buffer_length: The maximum size of the buffers to be used for in-memory
 * storage of metrics; see #EmtrEventRecorder:individual-buffer-length,
 * #EmtrEventRecorder:aggregate-buffer-length, and
 * #EmtrEventRecorder:sequence-buffer-length
 * @machine_id_provider: (allow-none): The #EmtrMachineIdProvider to supply the
 * machine ID, or %NULL to use the default; see
 * #EmtrEventRecorder:machine-id-provider
 *
 * Testing function for creating a new #EmtrEventRecorder in the C API.
 * You only need to use this if you are creating an event recorder with
 * nonstandard parameters for use in unit testing.
 *
 * Make sure to pass "test" for @environment if using in a test, and never pass
 * a live metrics proxy server URI for @proxy_server_uri.
 *
 * For all normal uses, you should use emtr_event_recorder_get_default()
 * instead.
 *
 * Returns: (transfer full): a new #EmtrEventRecorder.
 * Free with g_object_unref() if using C when done with it.
 */
EmtrEventRecorder *
emtr_event_recorder_new (guint                  network_send_interval,
                         gint                   version_number,
                         const gchar           *environment,
                         const gchar           *proxy_server_uri,
                         gint                   buffer_length,
                         EmtrMachineIdProvider *machine_id_provider)
{
  if (environment == NULL)
    g_error ("'environment' parameter was NULL. This is not allowed. No cookie for you.");
  if (proxy_server_uri == NULL)
    g_error ("'proxy_server_uri' parameter was NULL. This is not allowed. Go to your room.");

  const gchar *proxy_to_use = g_strdup (proxy_server_uri);
  const gchar *env_to_use = g_strdup (environment);
  g_object_ref (machine_id_provider);
  return g_object_new (EMTR_TYPE_EVENT_RECORDER,
                       "network-send-interval", network_send_interval,
                       "client-version-number", version_number,
                       "environment", env_to_use,
                       "proxy-server-uri", proxy_to_use,
                       "individual-buffer-length", buffer_length,
                       "aggregate-buffer-length", buffer_length,
                       "sequence-buffer-length", buffer_length,
                       "machine-id-provider", machine_id_provider,
                       NULL);
}

/**
 * emtr_event_recorder_get_default:
 *
 * Gets the event recorder object that you should use to record all metrics.
 *
 * Returns: (transfer none): the default #EmtrEventRecorder.
 * This object is owned by the metrics library; do not free it.
 */
EmtrEventRecorder *
emtr_event_recorder_get_default (void)
{
  static EmtrEventRecorder *singleton;
  G_LOCK_DEFINE_STATIC (singleton);

  G_LOCK (singleton);
  if (singleton == NULL)
    {
      EmtrMachineIdProvider *machine_provider = emtr_machine_id_provider_get_default ();
      singleton = g_object_new (EMTR_TYPE_EVENT_RECORDER, 
                                "machine-id-provider", machine_provider,
                                NULL);
      g_object_unref (machine_provider);
    }
  G_UNLOCK (singleton);

  return singleton;
}

/* Send either singular or aggregate event to DBus, currently a no-op. 
   num_events parameter is ignored if is_aggregate is FALSE. */
static void send_event_to_dbus (EmtrEventRecorderPrivate *priv,
                                uuid_t                    parsed_event_id,
                                GVariant                 *auxiliary_payload,
                                gint64                    relative_time,
                                gboolean                  is_aggregate,
                                gint                      num_events)
{
  GError *error = NULL;
  GVariantBuilder uuid_builder;
  get_uuid_builder (parsed_event_id, &uuid_builder);
  GVariant *event_id_variant = g_variant_builder_end (&uuid_builder);

  /* Variants sent to DBus are not allowed to be NULL or maybe types. */
  gboolean has_payload = auxiliary_payload != NULL;
  GVariant *maybe_auxiliary_payload = has_payload ?
    g_variant_new_variant (auxiliary_payload) : priv->empty_auxiliary_payload;
  gboolean success;
  if (is_aggregate)
    {
      success = emer_event_recorder_server_call_record_aggregate_event_sync (priv->dbus_proxy,
                                                                             getuid (),
                                                                             event_id_variant,
                                                                             num_events,
                                                                             relative_time,
                                                                             has_payload,
                                                                             maybe_auxiliary_payload,
                                                                             NULL /* GCancellable */,
                                                                             &error);
    }
  else
    {
      success = emer_event_recorder_server_call_record_singular_event_sync (priv->dbus_proxy,
                                                                            getuid (),
                                                                            event_id_variant,
                                                                            relative_time,
                                                                            has_payload,
                                                                            maybe_auxiliary_payload,
                                                                            NULL /* GCancellable */,
                                                                            &error);
    }
  if (!success)
    {
      g_warning ("Failed to send event to DBus client-side daemon: %s",
                 error->message);
      g_error_free (error);
    }
}
/**
 * emtr_event_recorder_record_event:
 * @self: (in): the event recorder
 * @event_id: (in): an RFC 4122 UUID representing the type of event that took
 * place
 * @auxiliary_payload: (allow-none) (in): miscellaneous data to associate with
 * the event
 *
 * Make a best-effort to record the fact that an event of type @event_id
 * happened at the current time. emtr-event-types.h is the registry for event
 * IDs. Optionally, associate arbitrary data, @auxiliary_payload, with this
 * particular instance of the event. Under no circumstances should
 * personally-identifiable information be included in the @auxiliary_payload or
 * @event_id. Large auxiliary payloads dominate the size of the event and should
 * therefore be used sparingly. Events for which precise timing information is
 * not required should instead be recorded using
 * emtr_event_recorder_record_events() to conserve bandwidth.
 *
 * At the discretion of the metrics system, the event may be discarded before
 * being reported to the metrics server. The event may take arbitrarily long to
 * reach the server and may be persisted unencrypted on the client for
 * arbitrarily long. There is no guarantee that the event is delivered via the
 * network; for example, it may instead be delivered manually on a USB drive.
 * No indication of successful or failed delivery is provided, and no
 * application should rely on successful delivery. The event will not be
 * aggregated with other events before reaching the server.
 */
void
emtr_event_recorder_record_event (EmtrEventRecorder *self,
                                  const gchar       *event_id,
                                  GVariant          *auxiliary_payload)
{
  /* Get the time before doing anything else because it will change during
  execution. */
  gint64 relative_time;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_time))
    {
      g_critical ("Getting relative timestamp failed.");
      return;
    }

  g_return_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self));
  g_return_if_fail (event_id != NULL);
  g_return_if_fail (auxiliary_payload == NULL || _IS_VARIANT(auxiliary_payload));

  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  if (!priv->recording_enabled)
    return;

  uuid_t parsed_event_id;
  if (!parse_event_id (event_id, parsed_event_id))
    return;

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  send_event_to_dbus (priv,
                      parsed_event_id,
                      auxiliary_payload,
                      relative_time,
                      FALSE, // Is not aggregate.
                      0); // Ignored: num_events

  g_mutex_lock (&(priv->event_buffer_lock));

  if (priv->num_events_buffered < priv->individual_buffer_length)
    {
      Event *event_buffer = priv->event_buffer;
      gint num_events_buffered = priv->num_events_buffered;

      if (G_LIKELY (num_events_buffered > 0))
        {
          Event *prev_event = event_buffer + num_events_buffered - 1;
          g_assert (relative_time >= prev_event->event_value.relative_time);
        }

      Event *event = event_buffer + num_events_buffered;
      priv->num_events_buffered++;
      EventValue event_value = {relative_time, auxiliary_payload};
      memcpy (event->event_id, parsed_event_id, UUID_LENGTH * sizeof (guchar));
      event->event_value = event_value;
    }

  g_mutex_unlock (&(priv->event_buffer_lock));
}

/**
 * emtr_event_recorder_record_events:
 * @self: (in): the event recorder
 * @event_id: (in): an RFC 4122 UUID representing the type of event that took
 * place
 * @num_events: (in): the number of times the event type took place
 * @auxiliary_payload: (allow-none) (in): miscellaneous data to associate with
 * the events
 *
 * Make a best-effort to record the fact that @num_events events of type
 * @event_id happened between the current time and the previous such recording.
 * emtr-event-types.h is the registry for event IDs. Optionally, associate
 * arbitrary data, @auxiliary_payload, with these particular instances of the
 * event. Under no circumstances should personally-identifiable information be
 * included in the @auxiliary_payload, the @event_id, or @num_events. Large
 * auxiliary payloads dominate the size of the event and should therefore be
 * used sparingly. Events for which precise timing information is required
 * should instead be recorded using emtr_event_recorder_record_event().
 *
 * At the discretion of the metrics system, the events may be discarded before
 * being reported to the metrics server. The events may take arbitrarily long to
 * reach the server and may be persisted unencrypted on the client for
 * arbitrarily long. There is no guarantee that the events are delivered via the
 * network; for example, they may instead be delivered manually on a USB drive.
 * No indication of successful or failed delivery is provided, and no
 * application should rely on successful delivery. To conserve bandwidth, the
 * events may be aggregated in a lossy fashion with other events with the same
 * @event_id before reaching the server.
 */
void
emtr_event_recorder_record_events (EmtrEventRecorder *self,
                                   const gchar       *event_id,
                                   gint64             num_events,
                                   GVariant          *auxiliary_payload)
{
  /* Get the time before doing anything else because it will change during
  execution. */
  gint64 relative_time;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_time))
    {
      g_critical ("Getting relative timestamp failed.");
      return;
    }

  g_return_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self));
  g_return_if_fail (event_id != NULL);
  g_return_if_fail (auxiliary_payload == NULL || _IS_VARIANT(auxiliary_payload));

  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  if (!priv->recording_enabled)
    return;

  uuid_t parsed_event_id;
  if (!parse_event_id (event_id, parsed_event_id))
    return;

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  send_event_to_dbus (priv,
                      parsed_event_id,
                      auxiliary_payload,
                      relative_time,
                      TRUE, // Is aggregate.
                      num_events);

  g_mutex_lock (&(priv->aggregate_buffer_lock));

  if (priv->num_aggregates_buffered < priv->aggregate_buffer_length)
    {
      Aggregate *aggregate_buffer = priv->aggregate_buffer;
      gint num_aggregates_buffered = priv->num_aggregates_buffered;

      if (G_LIKELY (num_aggregates_buffered > 0))
        {
          Aggregate *prev_aggregate = aggregate_buffer +
            num_aggregates_buffered - 1;
          g_assert (relative_time >=
                    prev_aggregate->event.event_value.relative_time);
        }

      Aggregate *aggregate = aggregate_buffer + num_aggregates_buffered;
      priv->num_aggregates_buffered++;
      EventValue event_value = {relative_time, auxiliary_payload};
      Event event;
      memcpy (event.event_id, parsed_event_id, UUID_LENGTH * sizeof (guchar));
      event.event_value = event_value;
      aggregate->event = event;
      aggregate->num_events = num_events;
    }

  g_mutex_unlock (&(priv->aggregate_buffer_lock));
}

/**
 * emtr_event_recorder_record_start:
 * @self: (in): the event recorder
 * @event_id: (in): an RFC 4122 UUID representing the type of event that took
 * place
 * @key: (allow-none) (in): the identifier used to associate the start of the
 * event with the stop and any progress
 * @auxiliary_payload: (allow-none) (in): miscellaneous data to associate with
 * the events
 *
 * Make a best-effort to record the fact that an event of type @event_id
 * started at the current time. The event's stop must be reported using
 * emtr_event_recorder_record_stop() or memory will be leaked.
 * emtr-event-types.h is the registry for event IDs. If starts and stops of
 * events of type @event_id can be nested, then @key should be used to
 * disambiguate the stop and any progress that corresponds to this start. For
 * example, if one were recording how long processes remained open, process IDs
 * would be a suitable choice for the @key. Within the lifetime of each process,
 * process IDs are unique within the scope of PROCESS_OPEN events. If starts and
 * stops of events of type @event_id can not be nested, then @key can be %NULL.
 *
 * Optionally, associate arbitrary data, @auxiliary_payload, with this
 * particular instance of the event. Under no circumstances should
 * personally-identifiable information be included in the @auxiliary_payload or
 * @event_id. Large auxiliary payloads dominate the size of the event and should
 * therefore be used sparingly. Events for which precise timing information is
 * not required should instead be recorded using
 * emtr_event_recorder_record_events() to conserve bandwidth.
 *
 * At the discretion of the metrics system, the event may be discarded before
 * being reported to the metrics server. However, an event start, the
 * corresponding stop, and any corresponding progress either will be delivered or
 * dropped atomically. The event may take arbitrarily long to reach the server
 * and may be persisted unencrypted on the client for arbitrarily long. There is
 * no guarantee that the event is delivered via the network; for example, it may
 * instead be delivered manually on a USB drive. No indication of successful or
 * failed delivery is provided, and no application should rely on successful
 * delivery. The event will not be aggregated with other events before reaching
 * the server.
 */
void
emtr_event_recorder_record_start (EmtrEventRecorder *self,
                                  const gchar       *event_id,
                                  GVariant          *key,
                                  GVariant          *auxiliary_payload)
{
  /* Validate inputs before acquiring the lock below to avoid verbose error
     handling that releases the lock and logs a custom error message. */
  g_return_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self));
  g_return_if_fail (event_id != NULL);
  g_return_if_fail (key == NULL || _IS_VARIANT (key));
  g_return_if_fail (auxiliary_payload == NULL ||
                    _IS_VARIANT (auxiliary_payload));

  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  /* Acquire this lock before getting the time so that event sequences are
     guaranteed to be chronologically sorted. */
  g_mutex_lock (&(priv->events_by_id_with_key_lock));

  // Get the time as soon as possible because it will change during execution.
  gint64 relative_time;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_time))
    {
      g_critical ("Getting relative timestamp failed.");
      goto finally;
    }

  if (!priv->recording_enabled)
    goto finally;

  uuid_t parsed_event_id;
  if (!parse_event_id (event_id, parsed_event_id))
    goto finally;

  key = get_normalized_form_of_variant (key);

  GVariant *event_id_with_key = combine_event_id_with_key (parsed_event_id,
                                                           key);

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  EventValue start_event_value = {relative_time, auxiliary_payload};
  GArray *event_values = g_array_sized_new (FALSE, FALSE, sizeof (EventValue),
                                            2);
  g_array_append_val (event_values, start_event_value);

  if (G_UNLIKELY (!g_hash_table_insert (priv->events_by_id_with_key,
                                        event_id_with_key, event_values)))
    {
      if (G_LIKELY (key != NULL))
        {
          gchar *key_as_string = g_variant_print (key, TRUE);
          g_variant_unref (key);

          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to start event of type %s with key %s "
                     "because there is already an unstopped start event with this "
                     "type and key.", event_id, key_as_string);

          g_free (key_as_string);
        }
      else
        {
          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to start event of type %s with NULL key "
                     "because there is already an unstopped start event with "
                     "this type and key.", event_id);
        }
      goto finally;
    }

  if (G_LIKELY (key != NULL))
    g_variant_unref (key);

finally:
  g_mutex_unlock (&(priv->events_by_id_with_key_lock));
}

/**
 * emtr_event_recorder_record_progress:
 * @self: (in): the event recorder
 * @event_id: (in): an RFC 4122 UUID representing the type of event that took
 * place
 * @key: (allow-none) (in): the identifier used to associate the event progress
 * with the start, stop, and any other progress
 * @auxiliary_payload: (allow-none) (in): miscellaneous data to associate with
 * the events
 *
 * Make a best-effort to record the fact that an event of type @event_id
 * progressed at the current time. May be called arbitrarily many times between
 * a corresponding start and stop. Behaves like
 * emtr_event_recorder_record_start().
 */
void
emtr_event_recorder_record_progress (EmtrEventRecorder *self,
                                     const gchar       *event_id,
                                     GVariant          *key,
                                     GVariant          *auxiliary_payload)
{
  /* Validate inputs before acquiring the lock below to avoid verbose error
     handling that releases the lock and logs a custom error message. */
  g_return_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self));
  g_return_if_fail (event_id != NULL);
  g_return_if_fail (key == NULL || _IS_VARIANT (key));
  g_return_if_fail (auxiliary_payload == NULL ||
                    _IS_VARIANT (auxiliary_payload));

  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  /* Acquire this lock before getting the time so that event sequences are
     guaranteed to be chronologically sorted. */
  g_mutex_lock (&(priv->events_by_id_with_key_lock));

  // Get the time as soon as possible because it will change during execution.
  gint64 relative_time;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_time))
    {
      g_critical ("Getting relative timestamp failed.");
      goto finally;
    }

  if (!priv->recording_enabled)
    goto finally;

  uuid_t parsed_event_id;
  if (!parse_event_id (event_id, parsed_event_id))
    goto finally;

  key = get_normalized_form_of_variant (key);

  GVariant *event_id_with_key = combine_event_id_with_key (parsed_event_id,
                                                           key);

  GArray *event_values =
    g_hash_table_lookup (priv->events_by_id_with_key, event_id_with_key);
  g_variant_unref (event_id_with_key);
  if (G_UNLIKELY (event_values == NULL))
    {
      if (G_LIKELY (key != NULL))
        {
          gchar *key_as_string = g_variant_print (key, TRUE);
          g_variant_unref (key);

          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to record progress for event of type %s "
                     "with key %s because there is no corresponding unstopped "
                     "start event.", event_id, key_as_string);

          g_free (key_as_string);
        }
      else
        {
          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to record progress for event of type %s "
                     "with NULL key because there is no corresponding "
                     "unstopped start event.", event_id);
        }
      goto finally;
    }

  if (G_LIKELY (key != NULL))
    g_variant_unref (key);

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  append_event_value (event_values, relative_time, auxiliary_payload);

finally:
  g_mutex_unlock (&(priv->events_by_id_with_key_lock));
}

/**
 * emtr_event_recorder_record_stop:
 * @self: (in): the event recorder
 * @event_id: (in): an RFC 4122 UUID representing the type of event that took
 * place
 * @key: (allow-none) (in): the identifier used to associate the stop of the
 * event with the start and any progress
 * @auxiliary_payload: (allow-none) (in): miscellaneous data to associate with
 * the events
 *
 * Make a best-effort to record the fact that an event of type @event_id
 * stopped at the current time. Behaves like emtr_event_recorder_record_start().
 */
void
emtr_event_recorder_record_stop (EmtrEventRecorder *self,
                                 const gchar       *event_id,
                                 GVariant          *key,
                                 GVariant          *auxiliary_payload)
{
  /* Validate inputs before acquiring the lock below to avoid verbose error
     handling that releases the lock and logs a custom error message. */
  g_return_if_fail (self != NULL && EMTR_IS_EVENT_RECORDER (self));
  g_return_if_fail (event_id != NULL);
  g_return_if_fail (key == NULL || _IS_VARIANT (key));
  g_return_if_fail (auxiliary_payload == NULL ||
                    _IS_VARIANT (auxiliary_payload));

  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  /* Acquire this lock before getting the time so that event sequences are
     guaranteed to be chronologically sorted. */
  g_mutex_lock (&(priv->events_by_id_with_key_lock));

  // Get the time as soon as possible because it will change during execution.
  gint64 relative_time;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_time))
    {
      g_critical ("Getting relative timestamp failed.");
      goto finally;
    }

  if (!priv->recording_enabled)
    goto finally;

  uuid_t parsed_event_id;
  if (!parse_event_id (event_id, parsed_event_id))
    goto finally;

  key = get_normalized_form_of_variant (key);

  GVariant *event_id_with_key = combine_event_id_with_key (parsed_event_id,
                                                           key);

  GArray *event_values =
    g_hash_table_lookup (priv->events_by_id_with_key, event_id_with_key);

  if (G_UNLIKELY (event_values == NULL))
    {
      g_variant_unref (event_id_with_key);
      if (G_LIKELY (key != NULL))
        {
          gchar *key_as_string = g_variant_print (key, TRUE);
          g_variant_unref (key);

          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to stop event of type %s with key %s "
                     "because there is no corresponding unstopped start "
                     "event.", event_id, key_as_string);

          g_free (key_as_string);
        }
      else
        {
          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to stop event of type %s with NULL key "
                     "because there is no corresponding unstopped start "
                     "event.", event_id);
        }
      goto finally;
    }

  if (G_LIKELY (key != NULL))
    g_variant_unref (key);

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  append_event_value (event_values, relative_time, auxiliary_payload);
  append_event_sequence_to_buffer (priv, event_id_with_key, event_values);

  GVariant *event_id_variant = g_variant_get_child_value (event_id_with_key, 0);
  send_event_sequence_to_dbus (priv, event_id_variant, event_values);
  g_variant_unref (event_id_variant);

  g_assert (g_hash_table_remove (priv->events_by_id_with_key,
                                 event_id_with_key));

finally:
  g_mutex_unlock (&(priv->events_by_id_with_key_lock));
}
