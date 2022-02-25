/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2021 Endless OS Foundation, LLC */

/*
 * This file is part of eos-event-recorder-daemon.
 *
 * eos-event-recorder-daemon is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 *
 * eos-event-recorder-daemon is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with eos-event-recorder-daemon.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "emer-aggregate-tally.h"

#include "shared/metrics-util.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <uuid/uuid.h>

#include <eosmetrics/eosmetrics.h>

#define UUID_LENGTH (sizeof (uuid_t) / sizeof (guchar))

// Generated with uuidgen

static const char *uuids[] = {
  "41d45e08-5e72-4c43-8cbf-ef37bb4411a4",
  "03177773-7513-4866-ae97-bb935f2c5384",
  "a692ce9c-8684-4d6b-97d5-07f39e0a8560",
};

struct AggregateEvent
{
  guint32 unix_user_id;
  GVariant *event_id;
  GVariant *aggregate_key;
  GVariant *payload;
};

struct Fixture
{
  EmerAggregateTally *tally;
  GFile *tally_folder;
};

static GVariant *
event_id_to_variant (const char *event_id)
{
  GVariantBuilder event_id_builder;
  uuid_t parsed_event_id;

  if (uuid_parse (event_id, parsed_event_id) != 0)
    return NULL;

  get_uuid_builder (parsed_event_id, &event_id_builder);

  return g_variant_new ("ay", &event_id_builder);
}

static struct AggregateEvent *
create_aggregate_event (guint32     unix_user_id,
                        const char *event_id,
                        GVariant   *aggregate_key,
                        GVariant   *payload)
{
  struct AggregateEvent *event;

  event = g_new0 (struct AggregateEvent, 1);
  event->unix_user_id = unix_user_id;
  event->event_id = g_variant_ref_sink (event_id_to_variant (event_id));
  event->aggregate_key = g_variant_ref_sink (g_variant_new_variant (aggregate_key));
  event->payload = g_variant_ref_sink (g_variant_new_variant (payload));

  return event;
}

static void
aggregate_event_free (struct AggregateEvent *event)
{
  g_clear_pointer (&event->event_id, g_variant_unref);
  g_clear_pointer (&event->aggregate_key, g_variant_unref);
  g_clear_pointer (&event->payload, g_variant_unref);
  g_free (event);
}

static void
teardown (struct Fixture *fixture,
          gconstpointer   dontuseme)
{
  g_clear_object (&fixture->tally);
}

static void
setup (struct Fixture *fixture,
       gconstpointer   dontuseme)
{
  const gchar *cache_dir = g_get_user_cache_dir ();
  fixture->tally = emer_aggregate_tally_new (cache_dir);
}

// Testing Cases

static void
test_aggregate_tally_new_succeeds (struct Fixture *fixture,
                                   gconstpointer   dontuseme)
{
  g_assert_nonnull (fixture->tally);
}

static void
test_aggregate_tally_store_events (struct Fixture *fixture,
                                   gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = NULL;
  g_autoptr(GError) error = NULL;
  struct AggregateEvent *event;

  datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  event = create_aggregate_event (1001, uuids[0],
                                  g_variant_new_string (""),
                                  g_variant_new_string (""));

  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    event->unix_user_id,
                                    event->event_id,
                                    event->aggregate_key,
                                    event->payload,
                                    1,
                                    datetime,
                                    g_get_monotonic_time (),
                                    &error);
  g_assert_no_error (error);

  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    event->unix_user_id,
                                    event->event_id,
                                    event->aggregate_key,
                                    event->payload,
                                    2,
                                    datetime,
                                    g_get_monotonic_time (),
                                    &error);
  g_assert_no_error (error);

  aggregate_event_free (event);
}

struct IterData {
  guint32 n_iterations;
  guint32 counter;
};

static EmerTallyIterResult
tally_iter_func (guint32     unix_user_id,
                 GVariant   *event_id,
                 GVariant   *aggregate_key,
                 GVariant   *payload,
                 guint32     counter,
                 const char *date,
                 gpointer    user_data)
{
  struct IterData *data = user_data;

  data->n_iterations++;
  data->counter += counter;

  return EMER_TALLY_ITER_CONTINUE;
}

static void
test_aggregate_tally_iter (struct Fixture *fixture,
                           gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = NULL;
  struct IterData data;
  int i;

  datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);

  // Add the same aggregate event multiple times. It must
  // result in a single aggregate event with the sum of
  // counters
  for (i = 0; i < 10; i++)
    {
      g_autoptr(GError) error = NULL;
      struct AggregateEvent *event;

      event = create_aggregate_event (1001, uuids[0],
                                      g_variant_new_string (""),
                                      g_variant_new_string (""));

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_DAILY_EVENTS,
                                        event->unix_user_id,
                                        event->event_id,
                                        event->aggregate_key,
                                        event->payload,
                                        1,
                                        datetime,
                                        g_get_monotonic_time (),
                                        &error);

      g_assert_no_error (error);

      aggregate_event_free (event);
    }

  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             &data);

  g_assert_cmpuint (data.n_iterations, ==, 1);
  g_assert_cmpuint (data.counter, ==, 10);

  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             &data);

  g_assert_cmpuint (data.n_iterations, ==, 0);
  g_assert_cmpuint (data.counter, ==, 0);
}

static void
test_aggregate_tally_large_counter_single (struct Fixture *fixture,
                                           gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = NULL;
  struct IterData data;

  datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);

  // Add an aggregate event with a counter too large to fit into a 32-bit
  // signed integer.
  g_autoptr(GError) error = NULL;
  struct AggregateEvent *event;

  event = create_aggregate_event (1001, uuids[0],
                                  g_variant_new_string (""),
                                  g_variant_new_string (""));

  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    event->unix_user_id,
                                    event->event_id,
                                    event->aggregate_key,
                                    event->payload,
                                    ((guint32) G_MAXINT32) + 1,
                                    datetime,
                                    g_get_monotonic_time (),
                                    &error);
  g_assert_no_error (error);
  aggregate_event_free (event);

  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             &data);

  g_assert_cmpuint (data.n_iterations, ==, 1);
  g_assert_cmpuint (data.counter, ==, ((guint32) G_MAXINT32) + 1);
}

static void
test_aggregate_tally_large_counter_add (struct Fixture *fixture,
                                        gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = NULL;
  struct IterData data;

  datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);

  // Add an aggregate event whose counter only just fits in a 32-bit signed
  // integer
  g_autoptr(GError) error = NULL;
  struct AggregateEvent *event;

  event = create_aggregate_event (1001, uuids[0],
                                  g_variant_new_string (""),
                                  g_variant_new_string (""));

  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    event->unix_user_id,
                                    event->event_id,
                                    event->aggregate_key,
                                    event->payload,
                                    (guint32) G_MAXINT32,
                                    datetime,
                                    g_get_monotonic_time (),
                                    &error);
  g_assert_no_error (error);

  // Now add 1 to it
  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    event->unix_user_id,
                                    event->event_id,
                                    event->aggregate_key,
                                    event->payload,
                                    1,
                                    datetime,
                                    g_get_monotonic_time (),
                                    &error);
  g_assert_no_error (error);

  aggregate_event_free (event);

  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             &data);

  g_assert_cmpuint (data.n_iterations, ==, 1);
  g_assert_cmpuint (data.counter, ==, ((guint32) G_MAXINT32) + 1);
}

static void
test_aggregate_tally_large_counter_upper_bound (struct Fixture *fixture,
                                                gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = NULL;
  size_t i;
  struct IterData data;

  datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);

  // The upper bound of an event's timer is 2 ** 32 - 1. Check that trying to
  // count above this limit is clamped at the limit.
  for (i = 0; i < 10; i++)
    {
      g_autoptr(GError) error = NULL;
      struct AggregateEvent *event;

      event = create_aggregate_event (1001, uuids[0],
                                      g_variant_new_string (""),
                                      g_variant_new_string (""));

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_DAILY_EVENTS,
                                        event->unix_user_id,
                                        event->event_id,
                                        event->aggregate_key,
                                        event->payload,
                                        G_MAXUINT32,
                                        datetime,
                                        g_get_monotonic_time (),
                                        &error);
      g_assert_no_error (error);

      aggregate_event_free (event);
    }

  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             &data);

  g_assert_cmpuint (data.n_iterations, ==, 1);
  g_assert_cmpuint (data.counter, ==, G_MAXUINT32);
}

static void
test_aggregate_tally_iter_before (struct Fixture *fixture,
                                  gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = NULL;
  struct IterData data;
  int i;

  datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);

  // Add the same aggregate event to different days in the past
  for (i = 0; i < 25; i++)
    {
      g_autoptr(GDateTime) past_datetime = NULL;
      g_autoptr(GError) error = NULL;
      struct AggregateEvent *event;

      past_datetime = g_date_time_add_days (datetime, -(i + 1));

      event = create_aggregate_event (1001, uuids[0],
                                      g_variant_new_string (""),
                                      g_variant_new_string (""));

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_DAILY_EVENTS,
                                        event->unix_user_id,
                                        event->event_id,
                                        event->aggregate_key,
                                        event->payload,
                                        1,
                                        past_datetime,
                                        g_get_monotonic_time (),
                                        &error);

      g_assert_no_error (error);

      aggregate_event_free (event);
    }

  // ... and in the future as well
  for (i = 0; i < 25; i++)
    {
      g_autoptr(GDateTime) future_datetime = NULL;
      g_autoptr(GError) error = NULL;
      struct AggregateEvent *event;

      future_datetime = g_date_time_add_days (datetime, i + 1);

      event = create_aggregate_event (1001, uuids[0],
                                      g_variant_new_string (""),
                                      g_variant_new_string (""));

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_DAILY_EVENTS,
                                        event->unix_user_id,
                                        event->event_id,
                                        event->aggregate_key,
                                        event->payload,
                                        1,
                                        future_datetime,
                                        g_get_monotonic_time (),
                                        &error);

      g_assert_no_error (error);

      aggregate_event_free (event);
    }

  /* Iterate but don't delete */
  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    &data);

  g_assert_cmpuint (data.n_iterations, ==, 25);
  g_assert_cmpuint (data.counter, ==, 25);

  /* Iterate again, and delete entries this time */
  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DELETE,
                                    tally_iter_func,
                                    &data);

  g_assert_cmpuint (data.n_iterations, ==, 25);
  g_assert_cmpuint (data.counter, ==, 25);

  /* There should be nothing else iterate now */
  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    &data);

  g_assert_cmpuint (data.n_iterations, ==, 0);
  g_assert_cmpuint (data.counter, ==, 0);

  // Now test previous months - add as much as 1 year to
  // the past
  for (i = 0; i < 12; i++)
    {
      g_autoptr(GDateTime) past_datetime = NULL;
      g_autoptr(GError) error = NULL;
      struct AggregateEvent *event;

      past_datetime = g_date_time_add_months (datetime, -(i + 1));

      event = create_aggregate_event (1001, uuids[0],
                                      g_variant_new_string (""),
                                      g_variant_new_string (""));

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_MONTHLY_EVENTS,
                                        event->unix_user_id,
                                        event->event_id,
                                        event->aggregate_key,
                                        event->payload,
                                        1,
                                        past_datetime,
                                        g_get_monotonic_time (),
                                        &error);

      g_assert_no_error (error);

      aggregate_event_free (event);
    }

  // ... and to the future as well
  for (i = 0; i < 12; i++)
    {
      g_autoptr(GDateTime) future_datetime = NULL;
      g_autoptr(GError) error = NULL;
      struct AggregateEvent *event;

      future_datetime = g_date_time_add_months (datetime, i + 1);

      event = create_aggregate_event (1001, uuids[0],
                                      g_variant_new_string (""),
                                      g_variant_new_string (""));

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_MONTHLY_EVENTS,
                                        event->unix_user_id,
                                        event->event_id,
                                        event->aggregate_key,
                                        event->payload,
                                        1,
                                        future_datetime,
                                        g_get_monotonic_time (),
                                        &error);

      g_assert_no_error (error);

      aggregate_event_free (event);
    }

  /* Iterate but don't delete */
  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    &data);

  g_assert_cmpuint (data.n_iterations, ==, 12);
  g_assert_cmpuint (data.counter, ==, 12);

  /* Iterate again, and delete entries this time */
  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DELETE,
                                    tally_iter_func,
                                    &data);

  g_assert_cmpuint (data.n_iterations, ==, 12);
  g_assert_cmpuint (data.counter, ==, 12);

  /* There should be nothing else iterate now */
  data = (struct IterData) { 0, 0 };
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    &data);

  g_assert_cmpuint (data.n_iterations, ==, 0);
  g_assert_cmpuint (data.counter, ==, 0);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

#define ADD_AGGREGATE_TALLY_TEST_FUNC(path, func) \
  g_test_add((path), struct Fixture, NULL, setup, (func), teardown)

  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/new-succeeds",
                                 test_aggregate_tally_new_succeeds);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/store-events",
                                 test_aggregate_tally_store_events);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/iter",
                                 test_aggregate_tally_iter);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/large-counter/single",
                                 test_aggregate_tally_large_counter_single);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/large-counter/add",
                                 test_aggregate_tally_large_counter_add);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/large-counter/upper-bound",
                                 test_aggregate_tally_large_counter_upper_bound);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/iter-before",
                                 test_aggregate_tally_iter_before);

#undef ADD_AGGREGATE_TALLY_TEST_FUNC


  return g_test_run ();
}