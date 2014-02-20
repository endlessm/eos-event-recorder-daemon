/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-event-recorder.h"

/**
 * SECTION:emtr-event-recorder
 * @title: Event Recorder
 * @short_description: Records metric events to metric system daemon.
 * @include: eosmetrics/eosmetrics.h
 *
 * The event recorder asynchronously sends metric events to the metric system
 * daemon via D-Bus. The system daemon then delivers metrics to the server on
 * a best-effort basis. No feedback is given regarding the outcome of delivery.
 *
 * This API may be called from Javascript as follows.
 *
 * |[
 * const EosMetrics = imports.gi.EosMetrics;
 * const GLib = imports.gi.GLib;
 * const MEANINGLESS_EVENT = 12;
 * const MEANINGLESS_AGGREGATED_EVENT = 7;
 * const MEANINGLESS_EVENT_WITH_AUX_DATA = 8;
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

typedef struct
{
  int ignored;
  // TODO: Add fields.
} EmtrEventRecorderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmtrEventRecorder, emtr_event_recorder, G_TYPE_OBJECT)

static void
emtr_event_recorder_finalize (GObject *object)
{
  EmtrEventRecorder *self = EMTR_EVENT_RECORDER (object);
  EmtrEventRecorderPrivate *priv = emtr_event_recorder_get_instance_private (self);

  G_OBJECT_CLASS (emtr_event_recorder_parent_class)->finalize (object);
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
 * @self: the event recorder
 * @event_id: the type of event that took place
 * @auxiliary_payload: miscellaneous data to associate with the event
 *
 * Make a best-effort to record the fact that an event of type @event_id
 * happened at the current time. Optionally, associate arbitrary data,
 * @auxiliary_payload, with this particular instance of the event. Under no
 * circumstances should personally-identifiable information be included in the
 * @auxiliary_payload or @event_id. Large auxiliary payloads dominate the size
 * of the event and should therefore be used sparingly. Events for which precise
 * timing information is not required should instead be recorded using
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
// TODO: Add link to mapping of event IDs to human-readable event descriptions.
void
emtr_event_recorder_record_event (EmtrEventRecorder *self,
                                  int                event_id,
                                  GVariant          *auxiliary_payload)
{
  // TODO: Implement.
}

/**
 * emtr_event_recorder_record_events:
 * @self: the event recorder
 * @event_id: the type of events that took place
 * @num_events: the number of times the event type took place
 * @auxiliary_payload: miscellaneous data to associate with the events
 *
 * Make a best-effort to record the fact that @num_events events of type
 * @event_id happened between the current time and the previous such recording.
 * Optionally, associate arbitrary data, @auxiliary_payload, with these
 * particular instances of the event. Under no circumstances should
 * personally-identifiable information be included in the @auxiliary_payload,
 * the @event_id, or @num_events. Large auxiliary payloads dominate the size of
 * the event and should therefore be used sparingly. Events for which precise
 * timing information is required should instead be recorded using
 * emtr_event_recorder_record_event().
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
// TODO: Add link to mapping of event IDs to human-readable event descriptions.
void
emtr_event_recorder_record_events (EmtrEventRecorder *self,
                                   int                event_id,
                                   signed long long   num_events,
                                   GVariant          *auxiliary_payload)
{
  // TODO: Implement.
}
