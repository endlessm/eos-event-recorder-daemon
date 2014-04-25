/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include "run-tests.h"

#include "eosmetrics/emtr-event-recorder.h"

#include <glib.h>

#define MEANINGLESS_EVENT "350ac4ff-3026-4c25-9e7e-e8103b4fd5d8"
#define MEANINGLESS_EVENT_2 "d936cd5c-08de-4d4e-8a87-8df1f4a33cba"

static void
test_event_recorder_new_succeeds (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  g_assert (event_recorder != NULL);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_event (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_event (event_recorder, MEANINGLESS_EVENT, NULL);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_events (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_events (event_recorder, MEANINGLESS_EVENT,
                                     G_GINT64_CONSTANT (12), NULL);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_start_stop (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT, NULL,
                                    NULL);
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT, NULL,
                                   NULL);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_progress (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT, NULL,
                                    NULL);
  emtr_event_recorder_record_progress (event_recorder, MEANINGLESS_EVENT, NULL,
                                       NULL);
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT, NULL,
                                   NULL);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_start_stop_with_key (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  GVariant *key = g_variant_new ("{sd}", "Power Level", 9320.73);
  g_variant_ref_sink (key);
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT, key,
                                    NULL);
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT, key,
                                   NULL);
  g_variant_unref (key);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_progress_with_key (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  GVariant *key =
    g_variant_new ("s", "NaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaNNaN BATMAN!!!");
  g_variant_ref_sink (key);
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT, key,
                                    NULL);
  for (gint i = 0; i < 10; ++i)
   {
    emtr_event_recorder_record_progress (event_recorder, MEANINGLESS_EVENT, key,
                                         NULL);
   }
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT, key,
                                   NULL);
  g_variant_unref (key);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_start_stop_with_floating_key (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT,
                                    g_variant_new ("i", 6170), NULL);
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT,
                                   g_variant_new ("i", 6170), NULL);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_progress_with_floating_key (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT,
                                    g_variant_new ("mv", NULL), NULL);
  emtr_event_recorder_record_progress (event_recorder, MEANINGLESS_EVENT,
                                       g_variant_new ("mv", NULL), NULL);
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT,
                                   g_variant_new ("mv", NULL), NULL);
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_auxiliary_payload (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_event (event_recorder, MEANINGLESS_EVENT,
                                    g_variant_new ("b", TRUE));
  emtr_event_recorder_record_events (event_recorder, MEANINGLESS_EVENT,
                                     G_GINT64_CONSTANT (7),
                                     g_variant_new ("b", FALSE));
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT, NULL,
                                    g_variant_new ("d", 5812.512));
  emtr_event_recorder_record_progress (event_recorder, MEANINGLESS_EVENT, NULL,
                                       g_variant_new ("md", NULL));
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT, NULL,
                                       g_variant_new ("(xt)",
                                       G_GINT64_CONSTANT (-82),
                                       G_GUINT64_CONSTANT (19)));
  g_object_unref (event_recorder);
}

static void
test_event_recorder_record_multiple_metric_sequences (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  GVariant *key = g_variant_new ("^ay", "Anna Breytenbach, Animal Whisperer");
  g_variant_ref_sink (key);
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT, key,
                                    NULL);
  emtr_event_recorder_record_progress (event_recorder, MEANINGLESS_EVENT, key,
                                       NULL);
  emtr_event_recorder_record_start (event_recorder, MEANINGLESS_EVENT_2, key,
                                    NULL);
  emtr_event_recorder_record_progress (event_recorder, MEANINGLESS_EVENT_2, key,
                                       NULL);
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT_2, key,
                                   NULL);
  emtr_event_recorder_record_stop (event_recorder, MEANINGLESS_EVENT, key,
                                   NULL);
  g_variant_unref (key);
  g_object_unref (event_recorder);
}

void
add_event_recorder_tests (void)
{
  g_test_add_func ("/event-recorder/new-succeeds",
                   test_event_recorder_new_succeeds);
  g_test_add_func ("/event-recorder/record-event",
                   test_event_recorder_record_event);
  g_test_add_func ("/event-recorder/record-events",
                   test_event_recorder_record_events);
  g_test_add_func ("/event-recorder/record-start-stop",
                   test_event_recorder_record_start_stop);
  g_test_add_func ("/event-recorder/record-progress",
                   test_event_recorder_record_progress);
  g_test_add_func ("/event-recorder/record-start-stop-with-key",
                   test_event_recorder_record_start_stop_with_key);
  g_test_add_func ("/event-recorder/record-progress-with-key",
                   test_event_recorder_record_progress_with_key);
  g_test_add_func ("/event-recorder/record-start-stop-with-floating-key",
                   test_event_recorder_record_start_stop_with_floating_key);
  g_test_add_func ("/event-recorder/record-progress-with-floating-key",
                   test_event_recorder_record_progress_with_floating_key);
  g_test_add_func ("/event-recorder/record-auxiliary-payload",
                   test_event_recorder_record_auxiliary_payload);
  g_test_add_func ("/event-recorder/record-multiple-metric-sequences",
                   test_event_recorder_record_multiple_metric_sequences);
}
