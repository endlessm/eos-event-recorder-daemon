/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L  /* for CLOCK_BOOTTIME */
#endif

#include "emtr-event-recorder.h"
#include "emer-event-recorder-server.h"
#include "shared/metrics-util.h"

#include <time.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <uuid/uuid.h>

/* Convenience macro to check that @ptr is a #GVariant */
#define _IS_VARIANT(ptr) (g_variant_is_of_type ((ptr), G_VARIANT_TYPE_ANY))

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
 * This API may be called from JavaScript as follows.
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

typedef struct EmtrEventRecorderPrivate
{
  /*
   * DBus doesn't support maybe types, so a boolean is used to indicate whether
   * the auxiliary_payload field should be ignored. A non-NULL auxiliary_payload
   * must be passed even when it will be ignored, and this is the arbitrary
   * variant that is used for that purpose.
   */
  GVariant *empty_auxiliary_payload;

  GHashTable * volatile events_by_id_with_key;
  GMutex events_by_id_with_key_lock;

  gboolean recording_enabled;

  EmerEventRecorderServer *dbus_proxy;
} EmtrEventRecorderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmtrEventRecorder, emtr_event_recorder, G_TYPE_OBJECT)

static void
emtr_event_recorder_finalize (GObject *object)
{
  EmtrEventRecorder *self = EMTR_EVENT_RECORDER (object);
  EmtrEventRecorderPrivate *priv =
    emtr_event_recorder_get_instance_private (self);

  g_hash_table_destroy (priv->events_by_id_with_key);
  g_mutex_clear (&(priv->events_by_id_with_key_lock));

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

static void
emtr_event_recorder_class_init (EmtrEventRecorderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = emtr_event_recorder_finalize;
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

/* Variants sent to DBus are not allowed to be NULL or maybe types. */
static GVariant *
get_time_with_maybe_variant (EmtrEventRecorderPrivate *priv,
                             gint64                    relative_time,
                             GVariant                 *payload)
{
  gboolean has_payload = (payload != NULL);
  GVariant *maybe_payload = has_payload ?
    payload : priv->empty_auxiliary_payload;
  return g_variant_new ("(xbv)", relative_time, has_payload, maybe_payload);
}

/* FIXME: The fact that this function needs @priv makes it a prime candidate for
refactoring. */
static void
append_event_to_sequence (EmtrEventRecorderPrivate *priv,
                          GArray                   *event_sequence,
                          gint64                    relative_time,
                          GVariant                 *auxiliary_payload)
{
  GVariant *curr_event_variant = get_time_with_maybe_variant (priv,
                                                              relative_time,
                                                              auxiliary_payload);
  g_array_append_val (event_sequence, curr_event_variant);
}

/*
 * Sends the corresponding event_sequence GVariant to D-Bus.
 */
static void
send_event_sequence_to_dbus (EmtrEventRecorderPrivate *priv,
                             GVariant                 *event_id,
                             GArray                   *event_sequence)
{
  GError *error = NULL;

  GVariantBuilder event_sequence_builder;
  g_variant_builder_init (&event_sequence_builder, G_VARIANT_TYPE ("a(xbv)"));
  for (int i = 0; i < event_sequence->len; i++)
    {
      GVariant *current = g_array_index (event_sequence, GVariant *, i);
      g_variant_builder_add_value (&event_sequence_builder, current);
    }
  GVariant *event_sequence_variant =
    g_variant_builder_end (&event_sequence_builder);

  if (!emer_event_recorder_server_call_record_event_sequence_sync (priv->dbus_proxy,
                                                                   getuid (),
                                                                   event_id,
                                                                   event_sequence_variant,
                                                                   NULL /* GCancellable */,
                                                                   &error))
    {
      g_warning ("Failed to send event to DBus client-side daemon: %s",
                 error->message);
      g_error_free (error);
    }
}

/* PUBLIC API */

/**
 * emtr_event_recorder_new:
 *
 * Testing function for creating a new #EmtrEventRecorder in the C API.
 * You only need to use this if you are creating an event recorder for use in
 * unit testing.
 *
 * For all normal uses, you should use emtr_event_recorder_get_default()
 * instead.
 *
 * Returns: (transfer full): a new #EmtrEventRecorder.
 * Free with g_object_unref() if using C when done with it.
 */
EmtrEventRecorder *
emtr_event_recorder_new (void)
{
  return g_object_new (EMTR_TYPE_EVENT_RECORDER, NULL);
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
    singleton = g_object_new (EMTR_TYPE_EVENT_RECORDER, NULL);
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
  GArray *event_sequence = g_array_sized_new (FALSE, FALSE,
                                              sizeof (GVariant *), 2);
  append_event_to_sequence (priv, event_sequence, relative_time,
                            auxiliary_payload);

  if (G_UNLIKELY (!g_hash_table_insert (priv->events_by_id_with_key,
                                        event_id_with_key, event_sequence)))
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
  GArray *event_sequence =
    g_hash_table_lookup (priv->events_by_id_with_key, event_id_with_key);
  g_variant_unref (event_id_with_key);

  if (event_sequence == NULL)
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

  append_event_to_sequence (priv, event_sequence, relative_time,
                            auxiliary_payload);

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
  GArray *event_sequence =
    g_hash_table_lookup (priv->events_by_id_with_key, event_id_with_key);

  if (event_sequence == NULL)
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

  append_event_to_sequence (priv, event_sequence, relative_time,
                            auxiliary_payload);

  GVariant *event_id_variant = g_variant_get_child_value (event_id_with_key, 0);
  send_event_sequence_to_dbus (priv, event_id_variant, event_sequence);
  g_variant_unref (event_id_variant);

  g_assert (g_hash_table_remove (priv->events_by_id_with_key,
                                 event_id_with_key));

finally:
  g_mutex_unlock (&(priv->events_by_id_with_key_lock));
}

#undef _IS_VARIANT
