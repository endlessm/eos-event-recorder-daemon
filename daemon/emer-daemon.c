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

// TODO: Once we have a production proxy server, update this constant
// accordingly.
/* The URI of the metrics production proxy server. Is kept in a #define to
prevent testing code from sending metrics to the production server. */
#define PRODUCTION_SERVER_URI "http://metrics-test.endlessm-sf.com:8080/"

typedef struct _EventValue
{
  // Time elapsed in nanoseconds from an unspecified starting point.
  gint64 relative_timestamp;

  GVariant *auxiliary_payload;
} EventValue;

typedef struct _SingularEvent
{
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
  uuid_t event_id;

  /*
   * The first element is the start event, the last element is the stop event,
   * and any elements in between are progress events. The elements are ordered
   * chronologically.
   */
  EventValue *event_values;

  gsize num_event_values;
} SequenceEvent;

typedef struct _EmerDaemonPrivate {
  /* Private storage for public properties */

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
uuid_from_gvariant (GVariant *event_id,
                    uuid_t    uuid)
{
  gsize event_id_length;
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
}

// Handles HTTP or HTTPS responses.
static void
handle_https_response (SoupRequest  *https_request,
                       GAsyncResult *result,
                       GVariant     *request_body)
{
  GError *error = NULL;
  GInputStream *response_stream = soup_request_send_finish (https_request,
                                                            result, &error);

  g_variant_unref (request_body);

  if (response_stream == NULL)
    {
      g_warning ("Error receiving metric HTTPS response: %s", error->message);
      g_error_free (error);
      return;
    }

  // TODO: Read and react to response.
  g_object_unref (response_stream);
}

static void
get_system_singulars_builder (EmerDaemonPrivate *priv,
                              GVariantBuilder   *system_singulars_builder)
{
  g_variant_builder_init (system_singulars_builder, G_VARIANT_TYPE ("a(ayxmv)"));

  g_mutex_lock (&priv->singular_buffer_lock);

  SingularEvent *singular_buffer = priv->singular_buffer;
  gint num_singulars = priv->num_singulars_buffered;
  for (gint i = 0; i < num_singulars; ++i)
    {
      SingularEvent *curr_singular = singular_buffer + i;
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_singular->event_id, &event_id_builder);
      g_variant_builder_add (system_singulars_builder, "(ayxmv)",
                             &event_id_builder,
                             curr_singular->event_value.relative_timestamp,
                             curr_singular->event_value.auxiliary_payload);
      free_singular_event (curr_singular);
    }
  priv->num_singulars_buffered = 0;

  g_mutex_unlock (&priv->singular_buffer_lock);
}

static void
get_system_aggregates_builder (EmerDaemonPrivate *priv,
                               GVariantBuilder   *system_aggregates_builder)
{
  g_variant_builder_init (system_aggregates_builder,
                          G_VARIANT_TYPE ("a(ayxxmv)"));

  g_mutex_lock (&priv->aggregate_buffer_lock);

  AggregateEvent *aggregate_buffer = priv->aggregate_buffer;
  gint num_aggregates = priv->num_aggregates_buffered;
  for (gint i = 0; i < num_aggregates; ++i)
    {
      AggregateEvent *curr_aggregate = aggregate_buffer + i;
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_aggregate->event.event_id, &event_id_builder);
      g_variant_builder_add (system_aggregates_builder, "(ayxxmv)",
                             &event_id_builder,
                             curr_aggregate->event.event_value.relative_timestamp,
                             curr_aggregate->num_events,
                             curr_aggregate->event.event_value.auxiliary_payload);
      free_singular_event (&curr_aggregate->event);
    }
  priv->num_aggregates_buffered = 0;

  g_mutex_unlock (&priv->aggregate_buffer_lock);
}

static void
get_system_sequences_builder (EmerDaemonPrivate *priv,
                              GVariantBuilder   *system_sequences_builder)
{
  g_variant_builder_init (system_sequences_builder,
                          G_VARIANT_TYPE ("a(aya(xmv))"));

  g_mutex_lock (&priv->sequence_buffer_lock);

  SequenceEvent *sequence_buffer = priv->sequence_buffer;
  gint num_sequences = priv->num_sequences_buffered;
  for (gint i = 0; i < num_sequences; ++i)
    {
      SequenceEvent *curr_event_sequence = sequence_buffer + i;
      GVariantBuilder event_values_builder;
      g_variant_builder_init (&event_values_builder, G_VARIANT_TYPE ("a(xmv)"));
      for (gint j = 0; j < curr_event_sequence->num_event_values; ++j)
        {
          EventValue *curr_event_value = curr_event_sequence->event_values + j;
          g_variant_builder_add (&event_values_builder, "(xmv)",
                                 curr_event_value->relative_timestamp,
                                 curr_event_value->auxiliary_payload);
        }
      GVariantBuilder event_id_builder;
      get_uuid_builder (curr_event_sequence->event_id, &event_id_builder);
      g_variant_builder_add (system_sequences_builder, "(aya(xmv))",
                             &event_id_builder, &event_values_builder);
      free_sequence_event (curr_event_sequence);
    }
  priv->num_sequences_buffered = 0;

  g_mutex_unlock (&priv->sequence_buffer_lock);
}

static GVariant *
create_request_body (EmerDaemonPrivate *priv)
{
  GVariantBuilder machine_id_builder;
  uuid_t machine_id;
  gboolean read_id = emer_machine_id_provider_get_id (priv->machine_id_provider,
                                                      machine_id);
  if (!read_id)
    return NULL;
  get_uuid_builder (machine_id, &machine_id_builder);

  GVariantBuilder user_events_builder;
  g_variant_builder_init (&user_events_builder,
                          G_VARIANT_TYPE ("a(aya(ayxmv)a(ayxxmv)a(aya(xmv)))"));
  // TODO: Populate user-specific events. Right now all metrics are considered
  // system-level.

  GVariantBuilder system_singulars_builder;
  get_system_singulars_builder (priv, &system_singulars_builder);

  GVariantBuilder system_aggregates_builder;
  get_system_aggregates_builder (priv, &system_aggregates_builder);

  GVariantBuilder system_sequences_builder;
  get_system_sequences_builder (priv, &system_sequences_builder);

  // Wait until the last possible moment to get the time of the network request
  // so that it can be used to measure network latency.
  gint64 relative_timestamp, absolute_timestamp;
  if (!get_current_time (CLOCK_BOOTTIME, &relative_timestamp) ||
      !get_current_time (CLOCK_REALTIME, &absolute_timestamp))
    return NULL;

  GVariant *request_body =
    g_variant_new ("(ixxaysa(aya(ayxmv)a(ayxxmv)a(aya(xmv)))"
                   "a(ayxmv)a(ayxxmv)a(aya(xmv)))", priv->client_version_number,
                   relative_timestamp, absolute_timestamp, &machine_id_builder,
                   priv->environment, &user_events_builder,
                   &system_singulars_builder, &system_aggregates_builder,
                   &system_sequences_builder);

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
get_https_request_uri (EmerDaemon   *self,
                       const guchar *data,
                       gsize         length)
{
  gchar *checksum_string = g_compute_checksum_for_data (G_CHECKSUM_SHA512, data,
                                                        length);
  EmerDaemonPrivate *priv = emer_daemon_get_instance_private (self);
  gchar *https_request_uri = g_strconcat (priv->proxy_server_uri,
                                          checksum_string, NULL);
  g_free (checksum_string);
  return https_request_uri;
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
  g_assert (serialized_request_body != NULL);
  gsize request_body_length = g_variant_get_size (request_body);
  gchar *https_request_uri =
    get_https_request_uri (self, serialized_request_body, request_body_length);

  GError *error = NULL;
  SoupRequestHTTP *https_request =
    soup_session_request_http (priv->http_session, "PUT", https_request_uri,
                               &error);
  g_free (https_request_uri);
  if (https_request == NULL)
    {
      g_warning ("Error creating metric HTTPS request: %s", error->message);
      g_error_free (error);
      return G_SOURCE_CONTINUE;
    }

  SoupMessage *https_message = soup_request_http_get_message (https_request);
  soup_message_set_request (https_message, "application/octet-stream",
                            SOUP_MEMORY_TEMPORARY, serialized_request_body,
                            request_body_length);
  soup_request_send_async (SOUP_REQUEST (https_request),
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
 * The following functions are private setters and getters for the properties of
 * EmerDaemon. These properties are write-only, construct-only, so these only
 * need to be internal.
 */

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
  priv->upload_events_timeout_source_id = g_timeout_add_seconds (seconds,
                                                                 (GSourceFunc) upload_events,
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
  priv->machine_id_provider = g_object_ref (machine_id_prov);
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
   * EmerDaemon:client-version-number:
   *
   * Network protocol version of the metrics client.
   */
  emer_daemon_props[PROP_CLIENT_VERSION_NUMBER] =
    g_param_spec_int ("client-version-number", "Client version number",
                      "Client network protocol version",
                      -1, G_MAXINT, 0,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

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
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_PROXY_SERVER_URI] =
    g_param_spec_string ("proxy-server-uri", "Proxy server URI",
                         "The URI to send metrics to",
                         PRODUCTION_SERVER_URI,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:machine-id-provider:
   *
   * An #EmerMachineIdProvider for retrieving the unique UUID of this machine.
   * If this property is not specified, the default machine ID provider (from
   * emer_machine_id_provider_get_default()) will be used.
   * You should only set this property to something else for testing purposes.
   */
  emer_daemon_props[PROP_MACHINE_ID_PROVIDER] =
    g_param_spec_object ("machine-id-provider", "Machine ID provider",
                         "Object providing unique machine ID",
                         EMER_TYPE_MACHINE_ID_PROVIDER,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /*
   * EmerDaemon:network-send-interval:
   *
   * The frequency with which the client will attempt a network send request, in
   * seconds.
   */
  emer_daemon_props[PROP_NETWORK_SEND_INTERVAL] =
    g_param_spec_uint ("network-send-interval", "Network send interval",
                       "Number of seconds until a network request is attempted",
                       0, G_MAXUINT, (60 * 60),
                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_SINGULAR_BUFFER_LENGTH] =
    g_param_spec_int ("singular-buffer-length", "Buffer length singular",
                       "The number of events allowed to be stored in the individual metric buffer",
                       -1, G_MAXINT, 2000,
                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_AGGREGATE_BUFFER_LENGTH] =
    g_param_spec_int ("aggregate-buffer-length", "Buffer length aggregate",
                      "The number of events allowed to be stored in the aggregate metric buffer",
                      -1, G_MAXINT, 2000,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

  /* Blurb string is good enough default documentation for this */
  emer_daemon_props[PROP_SEQUENCE_BUFFER_LENGTH] =
    g_param_spec_int ("sequence-buffer-length", "Buffer length sequence",
                      "The number of events allowed to be stored in the sequence metric buffer",
                      -1, G_MAXINT, 2000,
                      G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS);

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
                                   SOUP_SESSION_ADD_FEATURE_BY_TYPE,
                                   SOUP_TYPE_LOGGER,
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
  EmerMachineIdProvider *id_provider = emer_machine_id_provider_get_default ();
  return g_object_new (EMER_TYPE_DAEMON,
                       "machine-id-provider", id_provider,
                       NULL);
}

/*
 * emer_daemon_new_full:
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
emer_daemon_new_full (gint                   version_number,
                      const gchar           *environment,
                      guint                  network_send_interval,
                      const gchar           *proxy_server_uri,
                      EmerMachineIdProvider *machine_id_provider,
                      gint                   buffer_length)
{
  if (machine_id_provider == NULL)
    machine_id_provider = emer_machine_id_provider_get_default ();
  return g_object_new (EMER_TYPE_DAEMON,
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
      EventValue event_value = { relative_timestamp, payload };
      uuid_from_gvariant (event_id, singular->event_id);
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
      EventValue event_value = { relative_timestamp, payload };
      SingularEvent singular;
      uuid_from_gvariant (event_id, singular.event_id);
      singular.event_value = event_value;
      aggregate->event = singular;
      aggregate->num_events = num_events;
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

      uuid_from_gvariant (event_id, event_sequence->event_id);

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

          GVariant *payload = has_payload ? maybe_payload : NULL;
          EventValue event_value = { relative_timestamp, payload };
          event_sequence->event_values[index] = event_value;
        }

      event_sequence->num_event_values = num_events;
    }

  g_mutex_unlock (&priv->sequence_buffer_lock);
}
