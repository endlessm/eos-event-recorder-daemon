/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#include <eosmetrics/emtr-event-recorder.h>
#include "run-tests.h"

static void
test_event_recorder_new_succeeds (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  g_assert (event_recorder != NULL);
}

static void
test_event_recorder_record_event (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_event (event_recorder, 0, NULL);
  // TODO: Test functionality once implemented.
}

static void
test_event_recorder_record_events (void)
{
  EmtrEventRecorder *event_recorder = emtr_event_recorder_new ();
  emtr_event_recorder_record_events (event_recorder, 1, 12LL, NULL);
  // TODO: Test functionality once implemented.
}

void
add_event_recorder_tests (void)
{
  g_test_add_func ("/event-recorder/new", test_event_recorder_new_succeeds);
  g_test_add_func ("/event-recorder/record-event",
                   test_event_recorder_record_event);
  g_test_add_func ("/event-recorder/record-events",
                   test_event_recorder_record_events);
}
