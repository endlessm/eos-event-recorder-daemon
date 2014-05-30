/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#define _POSIX_C_SOURCE 200112L

#include "emtr-event-recorder.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gprintf.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>

/* Convenience macro to check that @ptr is a #GVariant */
#define _IS_VARIANT(ptr) (g_variant_is_of_type ((ptr), G_VARIANT_TYPE_ANY))

/*
 * Must be incremented every time the network protocol is changed so that the
 * proxy server can correctly handle both old and new clients while the updated
 * metrics package rolls out to all clients.
 */
#define CLIENT_VERSION 0

/*
 * Filepath at which the random UUID that persistently identifies this machine
 * is stored.
 * In order to protect the anonymity of our users, the ID stored in this file
 * must be randomly generated and not traceable back to the user's device.
 * See http://www.freedesktop.org/software/systemd/man/machine-id.html for more
 * details.
 */
#define MACHINE_ID_FILEPATH "/etc/machine-id"

/*
 * The expected size in bytes of the file located at MACHINE_ID_FILEPATH.
 * According to http://www.freedesktop.org/software/systemd/man/machine-id.html
 * the file should be 32 lower-case hexadecimal characters followed by a
 * newline character.
 */
#define MACHINE_ID_FILE_SIZE 33

/*
 * Specifies whether the metrics come from regular users in production,
 * employees/contractors developing EndlessOS, or automated tests. For now, we
 * consider all metrics to come from a development environment until we build
 * some confidence in the metrics system.
 */
#define ENVIRONMENT "dev"

/*
 * The maximum frequency with which an attempt to send metrics over the network
 * is made.
 */
#define NETWORK_SEND_INTERVAL_SECONDS (60 * 60)

/*
 * The number of elements in a uuid_t. uuid_t is assumed to be a fixed-length
 * array of guchar.
 */
#define UUID_LENGTH (sizeof (uuid_t) / sizeof (guchar))

/*
 * The maximum number of ordinary events that may be stored in RAM in the buffer
 * of events waiting to be sent to the metrics server.
 */
#define EVENT_BUFFER_LENGTH 2000

/*
 * The maximum number of aggregated events that may be stored in RAM in the
 * buffer of events waiting to be sent to the metrics server.
 */
#define AGGREGATE_BUFFER_LENGTH 2000

/*
 * The maximum number of event sequences that may be stored in RAM in the buffer
 * of event sequences waiting to be sent to the metrics server. Does not include
 * unstopped event sequences.
 */
#define SEQUENCE_BUFFER_LENGTH 2000

// The number of nanoseconds in one second.
#define NANOSECONDS_PER_SECOND 1000000000L

// TODO: Once we have a production proxy server, update this constant
// accordingly.
// The URI of the metrics production proxy server.
#define PROXY_PROD_SERVER_URI "http://metrics-test.endlessm-sf.com:8080/"

// The URI of the metrics test proxy server.
#define PROXY_TEST_SERVER_URI "http://metrics-test.endlessm-sf.com:8080/"

/*
 * Caches a random UUID stored in a file that persistently identifies this
 * machine. In order to protect the anonymity of our users, this ID must be
 * randomly generated and not traceable back to the user's device.
 */
static uuid_t machine_id;

// TODO: Re-evaluate whether this is necessary, or a GOnce (or nothing) would be
// more appropriate.
G_LOCK_DEFINE_STATIC (machine_id);

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
 * let eventRecorder = EosMetrics.EventRecorder.new();
 *
 * // Records a single instance of MEANINGLESS_EVENT along with the current
 * // time.
 * eventRecorder.prototype.record_event(MEANINGLESS_EVENT, null);
 *
 * // Records the fact that MEANINGLESS_AGGREGATED_EVENT occurred 23
 * // times since the last time it was recorded.
 * eventRecorder.prototype.record_events(MEANINGLESS_AGGREGATED_EVENT,
 *   23, null);
 *
 * // Records MEANINGLESS_EVENT_WITH_AUX_DATA along with some auxiliary data and
 * // the current time.
 * eventRecorder.prototype.record_event(MEANINGLESS_EVENT_WITH_AUX_DATA,
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

  if (priv->recording_enabled)
    {
      g_source_remove (priv->upload_events_timeout_source_id);

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
    }

  G_OBJECT_CLASS (emtr_event_recorder_parent_class)->finalize (object);
}

static void
emtr_event_recorder_class_init (EmtrEventRecorderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = emtr_event_recorder_finalize;
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

static gboolean
use_prod_server (void)
{
  return FALSE;
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
      g_warning ("Error receiving metric HTTPS response: %s\n", error->message);
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
      g_critical ("Attempt to get current time failed with error code: %d.\n",
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

  G_LOCK (machine_id);
  get_uuid_builder (machine_id, &machine_id_builder);
  G_UNLOCK (machine_id);

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
                   "a(ayxmv)a(ayxxmv)a(aya(xmv)))", CLIENT_VERSION,
                   relative_time, absolute_time, &machine_id_builder,
                   ENVIRONMENT, &user_events_builder, &system_events_builder,
                   &system_aggregates_builder, &system_event_sequences_builder);

  GVariant *big_endian_request_body = request_body;
  if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
    {
      big_endian_request_body = g_variant_byteswap (request_body);
      g_variant_unref (request_body);
    }
  else
    {
      g_assert (G_BYTE_ORDER == G_BIG_ENDIAN);
    }

  return big_endian_request_body;
}

static gchar *
get_https_request_uri (const guchar *data, gsize length)
{
  const gchar *proxy_server_uri = (use_prod_server ()) ?
    PROXY_PROD_SERVER_URI : PROXY_TEST_SERVER_URI;
  gchar *checksum_string = g_compute_checksum_for_data (G_CHECKSUM_SHA512, data,
                                                        length);
  gchar *https_request_uri = g_strconcat (proxy_server_uri, checksum_string,
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
  if (G_UNLIKELY (request_body == NULL))
    return G_SOURCE_CONTINUE;

  gconstpointer serialized_request_body = g_variant_get_data (request_body);
  g_assert (serialized_request_body != NULL);
  gsize request_body_length = g_variant_get_size (request_body);
  gchar *https_request_uri =
    get_https_request_uri (serialized_request_body, request_body_length);

  GError *error = NULL;
  SoupRequestHTTP *https_request =
    soup_session_request_http (priv->http_session, "PUT", https_request_uri,
                               &error);
  g_free (https_request_uri);
  if (G_UNLIKELY (https_request == NULL))
    {
      g_warning ("Error creating metric HTTPS request: %s\n", error->message);
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

/*
 * Returns a newly-allocated copy of uuid_sans_hyphens with hyphens inserted at
 * the appropriate positions as defined by uuid_unparse(3).
 * uuid_sans_hyphens is expected to be exactly 32 bytes, excluding the terminal
 * null byte.
 * Any extra bytes are ignored.
 * The returned string is guaranteed to be null-terminated.
 */
static gchar *
hyphenate_uuid (gchar *uuid_sans_hyphens)
{
  return g_strdup_printf ("%.8s-%.4s-%.4s-%.4s-%.12s", uuid_sans_hyphens,
                          uuid_sans_hyphens + 8, uuid_sans_hyphens + 12,
                          uuid_sans_hyphens + 16, uuid_sans_hyphens + 20);
}

static gboolean
read_machine_id (void)
{
  // The machine ID has already been read from disk; no need to read it again.
  if (machine_id != NULL)
    return TRUE;

  gchar *machine_id_sans_hyphens;
  gsize machine_id_sans_hyphens_length;
  GError *error = NULL;
  gboolean read_succeeded =
    g_file_get_contents (MACHINE_ID_FILEPATH, &machine_id_sans_hyphens,
                         &machine_id_sans_hyphens_length, &error);
  if (!read_succeeded)
    {
      g_critical ("Failed to read machine ID file (%s). Disabled metric "
                  "recording.\n", MACHINE_ID_FILEPATH);
      return FALSE;
    }

  if (strlen (machine_id_sans_hyphens) != machine_id_sans_hyphens_length)
    {
      g_critical ("Machine ID file (%s) contained null byte, but should be "
                  "hexadecimal. Disabled metric recording.\n",
                  MACHINE_ID_FILEPATH);
      return FALSE;
    }

  if (machine_id_sans_hyphens_length != MACHINE_ID_FILE_SIZE)
    {
      g_critical ("Machine ID file (%s) contained %" G_GSIZE_FORMAT " bytes, "
                  "but expected %d bytes. Disabled metric recording.\n",
                  MACHINE_ID_FILEPATH, machine_id_sans_hyphens_length,
                  MACHINE_ID_FILE_SIZE);
      return FALSE;
    }

  gchar *hyphenated_machine_id = hyphenate_uuid (machine_id_sans_hyphens);
  g_free (machine_id_sans_hyphens);

  G_LOCK (machine_id);
  int parse_failed = uuid_parse (hyphenated_machine_id, machine_id);
  G_UNLOCK (machine_id);

  g_free (hyphenated_machine_id);

  if (parse_failed != 0)
    {
      g_critical ("Machine ID file (%s) did not contain UUID. Disabled metric "
                  "recording.\n", MACHINE_ID_FILEPATH);
      return FALSE;
    }

  return TRUE;
}

static void
emtr_event_recorder_init (EmtrEventRecorder *self)
{
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  /*
   * If we can't read the machine ID, mark self a no-op event recorder, and
   * don't even initialize the rest of the private state.
   */
  priv->recording_enabled = read_machine_id ();
  if (!priv->recording_enabled)
    return;

  priv->event_buffer = g_new (Event, EVENT_BUFFER_LENGTH);
  priv->num_events_buffered = 0;
  g_mutex_init (&(priv->event_buffer_lock));

  priv->aggregate_buffer = g_new (Aggregate, AGGREGATE_BUFFER_LENGTH);
  priv->num_aggregates_buffered = 0;
  g_mutex_init (&(priv->aggregate_buffer_lock));

  priv->event_sequence_buffer = g_new (EventSequence, SEQUENCE_BUFFER_LENGTH);
  priv->num_event_sequences_buffered = 0;
  g_mutex_init (&(priv->event_sequence_buffer_lock));

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

  priv->upload_events_timeout_source_id =
    g_timeout_add_seconds (NETWORK_SEND_INTERVAL_SECONDS, upload_events, self);
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
                 "install uuid-runtime first.\n", unparsed_event_id);
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

static void
append_event_sequence_to_buffer (EmtrEventRecorderPrivate *priv,
                                 GVariant                 *event_id_with_key,
                                 GArray                   *event_values)
{
  g_mutex_lock (&(priv->event_sequence_buffer_lock));
  if (G_LIKELY (priv->num_event_sequences_buffered < SEQUENCE_BUFFER_LENGTH))
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
 *
 * Convenience function for creating a new #EmtrEventRecorder in the C API.
 *
 * Returns: (transfer full): a new #EmtrEventRecorder.
 * Free with g_object_unref() when done if using C.
 */
EmtrEventRecorder *
emtr_event_recorder_new (void)
{
  return g_object_new (EMTR_TYPE_EVENT_RECORDER, NULL);
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

  g_mutex_lock (&(priv->event_buffer_lock));

  if (G_LIKELY (priv->num_events_buffered < EVENT_BUFFER_LENGTH))
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
      auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);
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

  g_mutex_lock (&(priv->aggregate_buffer_lock));

  if (G_LIKELY (priv->num_aggregates_buffered < AGGREGATE_BUFFER_LENGTH))
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
      auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);
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
                     "type and key.\n", event_id, key_as_string);

          g_free (key_as_string);
        }
      else
        {
          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to start event of type %s with NULL key "
                     "because there is already an unstopped start event with "
                     "this type and key.\n", event_id);
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
                     "start event.\n", event_id, key_as_string);

          g_free (key_as_string);
        }
      else
        {
          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to record progress for event of type %s "
                     "with NULL key because there is no corresponding "
                     "unstopped start event.\n", event_id);
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
                     "event.\n", event_id, key_as_string);

          g_free (key_as_string);
        }
      else
        {
          // TODO: Make error message more helpful by printing the name of the
          // event as opposed to its UUID.
          g_warning ("Ignoring request to stop event of type %s with NULL key "
                     "because there is no corresponding unstopped start "
                     "event.\n", event_id);
        }
      goto finally;
    }

  if (G_LIKELY (key != NULL))
    g_variant_unref (key);

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  append_event_value (event_values, relative_time, auxiliary_payload);
  append_event_sequence_to_buffer (priv, event_id_with_key, event_values);
  g_assert (g_hash_table_remove (priv->events_by_id_with_key,
                                 event_id_with_key));

finally:
  g_mutex_unlock (&(priv->events_by_id_with_key_lock));
}
