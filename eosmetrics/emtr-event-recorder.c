/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#define _POSIX_C_SOURCE 200112L

#include "emtr-event-recorder.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <time.h>
#include <string.h>
#include <uuid/uuid.h>

/*
 * The maximum frequency with which an attempt to send metrics over the network
 * is made.
 */
#define NETWORK_SEND_INTERVAL_SECONDS (60 * 60)

/*
 * The maximum number of event sequences that may be stored in RAM in the buffer
 * of event sequences waiting to be sent to the metrics server. Does not include
 * unstopped event sequences.
 */
#define SEQUENCE_BUFFER_LENGTH 5000

// The number of nanoseconds in one second.
#define NANOSECONDS_PER_SECOND 1000000000L

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
  const gint64 relative_time;

  GVariant *auxiliary_payload;
} EventValue;

typedef struct EventSequence
{
  const uuid_t const event_id;
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
  EventSequence * volatile event_sequence_buffer;
  volatile gint num_event_sequences_buffered;
  GMutex event_sequence_buffer_lock;
  GHashTable * volatile events_by_id_with_key;
  GMutex events_by_id_with_key_lock;
  guint upload_events_timeout_source_id;
} EmtrEventRecorderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmtrEventRecorder, emtr_event_recorder, G_TYPE_OBJECT)

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
  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);

  g_assert (g_source_remove (private_state->upload_events_timeout_source_id));
  EventSequence *event_sequence_buffer = private_state->event_sequence_buffer;
  gint num_event_sequences = private_state->num_event_sequences_buffered;
  for (gint i = 0; i < num_event_sequences; ++i)
    free_event_sequence (event_sequence_buffer + i);

  g_free (event_sequence_buffer);
  g_mutex_clear (&(private_state->event_sequence_buffer_lock));
  g_hash_table_destroy (private_state->events_by_id_with_key);
  g_mutex_clear (&(private_state->events_by_id_with_key_lock));

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
upload_events (gpointer user_data)
{
  EmtrEventRecorder *self = (EmtrEventRecorder *) user_data;

  // Decrease the chances of a race with emtr_event_recorder_finalize.
  g_object_ref (self);

  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);
  g_mutex_lock (&(private_state->event_sequence_buffer_lock));

  EventSequence *event_sequence_buffer = private_state->event_sequence_buffer;
  gint num_event_sequences = private_state->num_event_sequences_buffered;
  for (gint i = 0; i < num_event_sequences; ++i)
    {
      // TODO: Correct for endianness if needed, serialize GVariants using
      // g_variant_get_data, and make an appropriately formatted HTTPS POST
      // request to the metrics server.
      free_event_sequence (event_sequence_buffer + i);
    }
  private_state->num_event_sequences_buffered = 0;

  g_mutex_unlock (&(private_state->event_sequence_buffer_lock));

  g_object_unref (self);

  return G_SOURCE_CONTINUE;
}

static void
emtr_event_recorder_init (EmtrEventRecorder *self)
{
  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);
  private_state->event_sequence_buffer = g_new (EventSequence,
                                                SEQUENCE_BUFFER_LENGTH);
  private_state->num_event_sequences_buffered = 0;
  g_mutex_init (&(private_state->event_sequence_buffer_lock));
  private_state->events_by_id_with_key =
    g_hash_table_new_full (general_variant_hash, g_variant_equal,
                           (GDestroyNotify) g_variant_unref,
                           (GDestroyNotify) g_array_unref);
  g_mutex_init (&(private_state->events_by_id_with_key_lock));
  private_state->upload_events_timeout_source_id =
    g_timeout_add_seconds (NETWORK_SEND_INTERVAL_SECONDS, upload_events, self);
}

static gboolean
inputs_are_valid (EmtrEventRecorder *self,
                  const gchar       *unparsed_event_id,
                  uuid_t             parsed_event_id,
                  gint64            *relative_time)
{
  // Get the time before doing anything else because it will change during
  // execution.
  struct timespec relative_timespec;
  int gettime_failed = clock_gettime (CLOCK_BOOTTIME, &relative_timespec);
  if (G_UNLIKELY (gettime_failed != 0))
    {
      int error_code = errno;
      g_critical ("Attempt to get current relative time failed with error "
                  "code: %d.\n", error_code);
      return FALSE;
    }

  // Ensure that the clock provides a time that can be safely represented in a
  // gint64 in nanoseconds.
  g_assert (relative_timespec.tv_sec >= (G_MININT64 / NANOSECONDS_PER_SECOND));
  g_assert (relative_timespec.tv_sec <= (G_MAXINT64 / NANOSECONDS_PER_SECOND));
  g_assert (relative_timespec.tv_nsec >= 0);
  g_assert (relative_timespec.tv_nsec < NANOSECONDS_PER_SECOND);

  // We already know that relative_timespec.tv_sec <=
  // (G_MAXINT64 / NANOSECONDS_PER_SECOND). This handles the edge case where
  // relative_timespec.tv_sec == (G_MAXINT64 / NANOSECONDS_PER_SECOND).
  g_assert ((relative_timespec.tv_sec <
             (G_MAXINT64 / NANOSECONDS_PER_SECOND)) ||
            (relative_timespec.tv_nsec <=
             (G_MAXINT64 % NANOSECONDS_PER_SECOND)));

  if (G_UNLIKELY (self == NULL))
    {
      g_warning ("self should be an instance of EmtrEventRecorder, not NULL.\n");
      return FALSE;
    }

  if (G_UNLIKELY (!EMTR_IS_EVENT_RECORDER (self)))
    {
      g_warning ("self should be an instance of EmtrEventRecorder.\n");
      return FALSE;
    }

  if (G_UNLIKELY (unparsed_event_id == NULL))
    {
      g_warning ("event_id should be a non-NULL instance of const gchar *.\n");
      return FALSE;
    }

  int parse_failed = uuid_parse (unparsed_event_id, parsed_event_id);
  if (G_UNLIKELY (parse_failed != 0))
    {
      g_warning ("Attempt to parse UUID \"%s\" failed. Make sure you created "
                 "this UUID with uuidgen -r. You may need to sudo apt-get "
                 "install uuid-runtime first.\n", unparsed_event_id);
      return FALSE;
    }

  *relative_time = ((gint64) relative_timespec.tv_nsec) +
    (NANOSECONDS_PER_SECOND * ((gint64) relative_timespec.tv_sec));

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
  GVariantBuilder *event_id_builder =
    g_variant_builder_new (G_VARIANT_TYPE ("ay"));
  size_t uuid_length = sizeof (uuid_t) / sizeof (event_id[0]);
  for (size_t i = 0; i < uuid_length; ++i)
    g_variant_builder_add (event_id_builder, "y", event_id[i]);

  GVariant *event_id_with_key = g_variant_new ("(aymv)", event_id_builder, key);
  g_variant_builder_unref (event_id_builder);
  return event_id_with_key;
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
append_event_sequence_to_buffer (EmtrEventRecorderPrivate *private_state,
                                 GVariant                 *event_id_with_key,
                                 GArray                   *event_values)
{
  g_mutex_lock (&(private_state->event_sequence_buffer_lock));
  if (G_LIKELY (private_state->num_event_sequences_buffered <
                SEQUENCE_BUFFER_LENGTH))
    {
      EventSequence *event_sequence =
        private_state->event_sequence_buffer +
        private_state->num_event_sequences_buffered;
      private_state->num_event_sequences_buffered++;
      g_variant_get_child (event_id_with_key, 0, "ay", event_sequence->event_id);
      event_sequence->key = g_variant_get_child_value (event_id_with_key, 1);
      event_sequence->event_values = g_new (EventValue, event_values->len);
      memcpy (event_sequence->event_values, event_values->data,
              event_values->len * sizeof (EventValue));
      event_sequence->num_event_values = event_values->len;
    }
  g_mutex_unlock (&(private_state->event_sequence_buffer_lock));
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
  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);
  // TODO: Grab appropriate lock.
  uuid_t parsed_event_id;
  gint64 relative_time;
  g_return_if_fail (inputs_are_valid (self, event_id, parsed_event_id,
                                      &relative_time));
  // TODO: Implement.
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
  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);
  // TODO: Grab appropriate lock.
  uuid_t parsed_event_id;
  gint64 relative_time;
  g_return_if_fail (inputs_are_valid (self, event_id, parsed_event_id,
                                      &relative_time));
  // TODO: Implement.
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
  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);
  g_mutex_lock (&(private_state->events_by_id_with_key_lock));
  uuid_t parsed_event_id;
  gint64 relative_time;
  g_return_if_fail (inputs_are_valid (self, event_id, parsed_event_id,
                                      &relative_time));

  key = get_normalized_form_of_variant (key);

  GVariant *event_id_with_key = combine_event_id_with_key (parsed_event_id,
                                                           key);

  if (G_UNLIKELY (g_hash_table_contains (private_state->events_by_id_with_key,
                                         event_id_with_key)))
    {
      g_mutex_unlock (&(private_state->events_by_id_with_key_lock));
      g_variant_unref (event_id_with_key);
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
      return;
    }

  if (G_LIKELY (key != NULL))
    g_variant_unref (key);

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  EventValue start_event_value = {relative_time, auxiliary_payload};
  GArray *event_values = g_array_sized_new (FALSE, FALSE, sizeof (EventValue),
                                            2);
  g_array_append_val (event_values, start_event_value);

  // TODO: Upgrade to a version of GLib in which g_hash_table_insert returns
  // whether the key already existed, and avoid the call to
  // g_hash_table_contains.
  g_hash_table_insert (private_state->events_by_id_with_key, event_id_with_key,
                       event_values);

  g_mutex_unlock (&(private_state->events_by_id_with_key_lock));
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
  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);
  g_mutex_lock (&(private_state->events_by_id_with_key_lock));
  uuid_t parsed_event_id;
  gint64 relative_time;
  g_return_if_fail (inputs_are_valid (self, event_id, parsed_event_id,
                                      &relative_time));

  key = get_normalized_form_of_variant (key);

  GVariant *event_id_with_key = combine_event_id_with_key (parsed_event_id,
                                                           key);

  GArray *event_values =
    g_hash_table_lookup (private_state->events_by_id_with_key,
                         event_id_with_key);
  g_variant_unref (event_id_with_key);
  if (G_UNLIKELY (event_values == NULL))
    {
      g_mutex_unlock (&(private_state->events_by_id_with_key_lock));
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
      return;
    }

  if (G_LIKELY (key != NULL))
    g_variant_unref (key);

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  append_event_value (event_values, relative_time, auxiliary_payload);
  g_mutex_unlock (&(private_state->events_by_id_with_key_lock));
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
  EmtrEventRecorderPrivate *private_state =
    emtr_event_recorder_get_instance_private (self);
  g_mutex_lock (&(private_state->events_by_id_with_key_lock));
  uuid_t parsed_event_id;
  gint64 relative_time;
  g_return_if_fail (inputs_are_valid (self, event_id, parsed_event_id,
                                      &relative_time));

  key = get_normalized_form_of_variant (key);

  GVariant *event_id_with_key = combine_event_id_with_key (parsed_event_id,
                                                           key);

  GArray *event_values =
    g_hash_table_lookup (private_state->events_by_id_with_key,
                         event_id_with_key);

  if (G_UNLIKELY (event_values == NULL))
    {
      g_mutex_unlock (&(private_state->events_by_id_with_key_lock));
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
      return;
    }

  if (G_LIKELY (key != NULL))
    g_variant_unref (key);

  auxiliary_payload = get_normalized_form_of_variant (auxiliary_payload);

  append_event_value (event_values, relative_time, auxiliary_payload);
  append_event_sequence_to_buffer (private_state, event_id_with_key,
                                   event_values);
  g_assert (g_hash_table_remove (private_state->events_by_id_with_key,
                                 event_id_with_key));
  g_mutex_unlock (&(private_state->events_by_id_with_key_lock));
}
