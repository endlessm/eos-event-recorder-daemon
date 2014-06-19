/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

/* For CLOCK_BOOTTIME */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L
#error "This code requires _POSIX_C_SOURCE to be 200112L or later."
#endif

#include <string.h>
#include <time.h>

#include <libsoup/soup.h>
#include <uuid/uuid.h>

#include "emer-daemon.h"
#include "emer-machine-id-provider.h"
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

typedef struct _EventValue
{
  // Time elapsed in nanoseconds from an unspecified starting point.
  gint64 relative_timestamp;

  GVariant *auxiliary_payload;
} EventValue;

typedef struct _SingularEvent
{
  guint32 user_id;
  uuid_t event_id;
  EventValue event_value;
} SingularEvent;

typedef struct _AggregateEvent
{
  SingularEvent event;
  gint64 num_events;
} AggregateEvent;

typedef struct _SequenceEvent
{
  guint32 user_id;
  uuid_t event_id;

  /*
   * The first element is the start event, the last element is the stop event,
   * and any elements in between are progress events. The elements are ordered
   * chronologically.
   */
  EventValue *event_values;

  gsize num_event_values;
} SequenceEvent;

typedef struct _NetworkCallbackData
{
  EmerDaemon *daemon;
  GVariant *request_body;
  gint attempt_num;
} NetworkCallbackData;

typedef struct _EmerDaemonPrivate {
  /* Private storage for public properties */

  GRand *rand;

  gint client_version_number;

  gchar *environment;

  guint network_send_interval_seconds;
  guint upload_events_timeout_source_id;

  SoupSession *http_session;
  gchar *proxy_server_uri;

  EmerMachineIdProvider *machine_id_provider;

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
  PROP_ENVIRONMENT,
  PROP_NETWORK_SEND_INTERVAL,
  PROP_PROXY_SERVER_URI,
  PROP_MACHINE_ID_PROVIDER,
  PROP_SINGULAR_BUFFER_LENGTH,
  PROP_AGGREGATE_BUFFER_LENGTH,
  PROP_SEQUENCE_BUFFER_LENGTH,
  NPROPS
};

static GParamSpec *emer_daemon_props[NPROPS] = { NULL, };

static void
free_singular_event (SingularEvent *singular)
{
  GVariant *auxiliary_payload = singular->event_value.auxiliary_payload;
  if (auxiliary_payload != NULL)
    g_variant_unref (auxiliary_payload);
}

static void
free_sequence_event (SequenceEvent *sequence)
{
  for (gint i = 0; i < sequence->num_event_values; ++i)
    {
      GVariant *curr_auxiliary_payload =
        sequence->event_values[i].auxiliary_payload;
      if (curr_auxiliary_payload != NULL)
        g_variant_unref (curr_auxiliary_payload);
    }

  g_free (sequence->event_values);
}

static void
free_network_callback_data (NetworkCallbackData *callback_data)
{
  g_variant_unref (callback_data->request_body);
  g_free (callback_data);
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
  gulong randomized_backoff_sec = random_factor * (gdouble) base_backoff_sec;
  g_usleep (G_USEC_PER_SEC * randomized_backoff_sec);
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

static GVariant *
get_updated_request_body (GVariant *request_body)
{
  gint32 client_version_number;
  GVariantIter *machine_id_iter;
  gchar *environment;
  GVariantIter *singulars_iter, *aggregates_iter, *sequences_iter;
  g_variant_get (request_body, "(ixxaysa(uayxmv)a(uayxxmv)a(uaya(xmv)))",
                 &client_version_number, NULL, NULL, &machine_id_iter,
                 &environment, &singulars_iter, &aggregates_iter,
                 &sequences_iter);

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
  // TODO: True up the relative time across boots.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_timestamp) ||
      !get_current_time (CLOCK_REALTIME, &absolute_timestamp))
    return NULL;

  GVariant *updated_request_body =
    g_variant_new ("(ixxaysa(uayxmv)a(uayxxmv)a(uaya(xmv)))",
                   client_version_number, relative_timestamp,
                   absolute_timestamp, &machine_id_builder, environment,
                   &singulars_builder, &aggregates_builder, &sequences_builder);
  g_free (environment);

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

  if (++callback_data->attempt_num >= NETWORK_ATTEMPT_LIMIT)
    {
      g_warning ("Maximum number of network attempts (%d) reached. Dropping "
                 "metrics.", callback_data->attempt_num);
      free_network_callback_data (callback_data);
      return;
    }

  if (SOUP_STATUS_IS_TRANSPORT_ERROR (status_code) ||
      SOUP_STATUS_IS_CLIENT_ERROR (status_code) ||
      SOUP_STATUS_IS_SERVER_ERROR (status_code))
    {
      EmerDaemon *self = callback_data->daemon;
      EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
      backoff (priv->rand, callback_data->attempt_num);
      GVariant *updated_request_body =
        get_updated_request_body (callback_data->request_body);
      g_variant_unref (callback_data->request_body);

      if (updated_request_body == NULL)
        {
          g_warning ("Could not update network request with current "
                     "timestamps. Dropping metrics.");
          free_network_callback_data (callback_data);
          return;
        }

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
get_singulars_builder (EmerDaemonPrivate *priv,
                       GVariantBuilder   *singulars_builder)
{
  g_variant_builder_init (singulars_builder, G_VARIANT_TYPE ("a(uayxmv)"));

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
      free_singular_event (curr_singular);
    }
  priv->num_singulars_buffered = 0;

  g_mutex_unlock (&priv->singular_buffer_lock);
}

static void
get_aggregates_builder (EmerDaemonPrivate *priv,
                        GVariantBuilder   *aggregates_builder)
{
  g_variant_builder_init (aggregates_builder, G_VARIANT_TYPE ("a(uayxxmv)"));

  g_mutex_lock (&priv->aggregate_buffer_lock);

  AggregateEvent *aggregate_buffer = priv->aggregate_buffer;
  gint num_aggregates = priv->num_aggregates_buffered;
  for (gint i = 0; i < num_aggregates; ++i)
    {
      AggregateEvent *curr_aggregate = aggregate_buffer + i;
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_aggregate->event.event_id, &event_id_builder);
      SingularEvent curr_event = curr_aggregate->event;
      EventValue curr_event_value = curr_event.event_value;
      g_variant_builder_add (aggregates_builder, "(uayxxmv)",
                             curr_event.user_id,
                             &event_id_builder,
                             curr_event_value.relative_timestamp,
                             curr_aggregate->num_events,
                             curr_event_value.auxiliary_payload);
      free_singular_event (&curr_aggregate->event);
    }
  priv->num_aggregates_buffered = 0;

  g_mutex_unlock (&priv->aggregate_buffer_lock);
}

static void
get_sequences_builder (EmerDaemonPrivate *priv,
                       GVariantBuilder   *sequences_builder)
{
  g_variant_builder_init (sequences_builder, G_VARIANT_TYPE ("a(uaya(xmv))"));

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
      free_sequence_event (curr_sequence);
    }
  priv->num_sequences_buffered = 0;

  g_mutex_unlock (&priv->sequence_buffer_lock);
}

static GVariant *
create_request_body (EmerDaemonPrivate *priv)
{
  uuid_t machine_id;
  gboolean read_id = emer_machine_id_provider_get_id (priv->machine_id_provider,
                                                      machine_id);
  if (!read_id)
    return NULL;

  GVariantBuilder machine_id_builder;
  get_uuid_builder (machine_id, &machine_id_builder);

  GVariantBuilder singulars_builder;
  get_singulars_builder (priv, &singulars_builder);

  GVariantBuilder aggregates_builder;
  get_aggregates_builder (priv, &aggregates_builder);

  GVariantBuilder sequences_builder;
  get_sequences_builder (priv, &sequences_builder);

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  // TODO: True up the relative time across boots.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_timestamp) ||
      !get_current_time (CLOCK_REALTIME, &absolute_timestamp))
    return NULL;

  GVariant *request_body =
    g_variant_new ("(ixxaysa(uayxmv)a(uayxxmv)a(uaya(xmv)))",
                   priv->client_version_number, relative_timestamp,
                   absolute_timestamp, &machine_id_builder, priv->environment,
                   &singulars_builder, &aggregates_builder, &sequences_builder);

  GVariant *little_endian_request_body = request_body;
  if (G_BYTE_ORDER == G_BIG_ENDIAN)
    {
      little_endian_request_body = g_variant_byteswap (request_body);
      g_variant_unref (request_body);
    }
  else if (G_BYTE_ORDER != G_LITTLE_ENDIAN)
    {
      g_error ("This machine is middle endian, which is not supported by the "
               "metrics system.");
    }

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

static gboolean
upload_events (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

  /* Decrease the chances of a race with emer_daemon_finalize. */
  g_object_ref (self);

  GVariant *request_body = create_request_body (priv);
  if (request_body == NULL)
    return G_SOURCE_CONTINUE;

  gconstpointer serialized_request_body = g_variant_get_data (request_body);
  if (serialized_request_body == NULL)
    {
      g_warning ("Could not serialize network request body.");
      return G_SOURCE_CONTINUE;
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
set_environment (EmerDaemon  *self,
                 const gchar *env)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->environment = g_strdup (env);
}

static gchar *
get_environment (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->environment;
}

static void
set_network_send_interval (EmerDaemon *self,
                           guint       seconds)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  priv->network_send_interval_seconds = seconds;
  priv->upload_events_timeout_source_id =
    g_timeout_add_seconds (seconds, (GSourceFunc) upload_events, self);
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

  priv->machine_id_provider = g_object_ref_sink (machine_id_prov);
}

static EmerMachineIdProvider *
get_machine_id_provider (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  return priv->machine_id_provider;
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

    case PROP_ENVIRONMENT:
      g_value_set_string (value, get_environment (self));
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

    case PROP_ENVIRONMENT:
      set_environment (self, g_value_get_string (value));
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

  g_rand_free (priv->rand);
  g_free (priv->environment);
  g_free (priv->proxy_server_uri);
  g_clear_object(&priv->machine_id_provider);

  SingularEvent *singular_buffer = priv->singular_buffer;
  gint num_singulars = priv->num_singulars_buffered;
  for (gint i = 0; i < num_singulars; ++i)
    free_singular_event (singular_buffer + i);

  g_free (singular_buffer);
  g_mutex_clear (&priv->singular_buffer_lock);

  AggregateEvent *aggregate_buffer = priv->aggregate_buffer;
  gint num_aggregates = priv->num_aggregates_buffered;
  for (gint i = 0; i < num_aggregates; ++i)
    free_singular_event (&aggregate_buffer[i].event);

  g_free (aggregate_buffer);
  g_mutex_clear (&priv->aggregate_buffer_lock);

  SequenceEvent *sequence_buffer = priv->sequence_buffer;
  gint num_sequences = priv->num_sequences_buffered;
  for (gint i = 0; i < num_sequences; ++i)
    free_sequence_event (sequence_buffer + i);

  g_free (sequence_buffer);
  g_mutex_clear (&priv->sequence_buffer_lock);

  G_OBJECT_CLASS (emer_daemon_parent_class)->finalize (object);
}

static void
emer_daemon_class_init (EmerDaemonClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

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

  /*
   * EmerDaemon:environment:
   *
   * Specifies what kind of user or system the metrics come from, so that data
   * analysis can exclude metrics from tests or developers' systems.
   *
   * Valid values are "dev", "test", and "prod".
   * The "prod" value, indicating production, should not be used outside of
   * a production system.
   */
  emer_daemon_props[PROP_ENVIRONMENT] =
    g_param_spec_string ("environment", "Environment",
                         "Specifies what kind of user the metrics come from",
                         "dev",
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
                       -1, G_MAXINT, 200,
                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                       G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_AGGREGATE_BUFFER_LENGTH] =
    g_param_spec_int ("aggregate-buffer-length", "Buffer length aggregate",
                      "The number of events allowed to be stored in the "
                      "aggregate metric buffer",
                      -1, G_MAXINT, 200,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                      G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_SEQUENCE_BUFFER_LENGTH] =
    g_param_spec_int ("sequence-buffer-length", "Buffer length sequence",
                      "The number of events allowed to be stored in the "
                      "sequence metric buffer",
                      -1, G_MAXINT, 200,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                      G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS, emer_daemon_props);
}

static void
emer_daemon_init (EmerDaemon *self)
{
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);

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
 * Creates a new EOS Metrics Daemon.
 *
 * Returns: (transfer full): a new #EmerDaemon.
 */
EmerDaemon *
emer_daemon_new (void)
{
  return g_object_new (EMER_TYPE_DAEMON,
                       NULL);
}

/*
 * emer_daemon_new_full:
 * @rand: (allow-none): random number generator to use for randomized
 *   exponential backoff, or %NULL to use the default
 * @version_number: client version of the network protocol
 * @environment: environment of the machine
 * @network_send_interval: frequency with which the client will attempt a
 *   network send request
 * @proxy_server_uri: URI to use
 * @machine_id_provider: (allow-none): The #EmerMachineIdProvider to supply the
 *   machine ID, or %NULL to use the default
 * @buffer_length: The maximum size of the buffers to be used for in-memory
 *   storage of metrics
 *
 * Testing function for creating a new EOS Metrics daemon.
 * You should only need to use this for unit testing.
 *
 * Make sure to pass "test" for @environment if using in a test, and never pass
 * a live metrics proxy server URI for @proxy_server_uri.
 *
 * Returns: (transfer full): a new #EmerDaemon.
 */
EmerDaemon *
emer_daemon_new_full (GRand                 *rand,
                      gint                   version_number,
                      const gchar           *environment,
                      guint                  network_send_interval,
                      const gchar           *proxy_server_uri,
                      EmerMachineIdProvider *machine_id_provider,
                      gint                   buffer_length)
{
  return g_object_new (EMER_TYPE_DAEMON,
                       "random-number-generator", rand,
                       "client-version-number", version_number,
                       "environment", environment,
                       "network-send-interval", network_send_interval,
                       "proxy-server-uri", proxy_server_uri,
                       "machine-id-provider", machine_id_provider,
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

          GVariant *nullable_payload = has_payload ? maybe_payload : NULL;
          EventValue event_value = { relative_timestamp, nullable_payload };
          event_sequence->event_values[index] = event_value;
        }

      g_variant_unref (event_values);
      event_sequence->num_event_values = num_events;
    }

  g_mutex_unlock (&priv->sequence_buffer_lock);
}
