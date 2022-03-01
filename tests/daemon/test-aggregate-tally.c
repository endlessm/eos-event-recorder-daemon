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

static const char *uuid_strs[] = {
  "41d45e08-5e72-4c43-8cbf-ef37bb4411a4",
  "03177773-7513-4866-ae97-bb935f2c5384",
  "a692ce9c-8684-4d6b-97d5-07f39e0a8560",
};
static uuid_t uuids[G_N_ELEMENTS(uuid_strs)];

typedef struct
{
  guint32     unix_user_id;
  uuid_t      event_id;
  GVariant   *aggregate_key;
  GVariant   *payload;
  guint32     counter;
  char *      date;
} AggregateEvent;

static AggregateEvent *
aggregate_event_new (guint32     unix_user_id,
                     uuid_t      event_id,
                     GVariant   *aggregate_key,
                     GVariant   *payload,
                     guint32     counter,
                     const char *date)
{
  AggregateEvent *e = g_new0 (AggregateEvent, 1);

  e->unix_user_id = unix_user_id;
  uuid_copy (e->event_id, event_id);
  e->aggregate_key = g_variant_ref (aggregate_key);
  e->payload = payload ? g_variant_ref (payload) : NULL;
  e->counter = counter;
  e->date = g_strdup (date);

  return e;
}

/* The argument has type gpointer to allow this function to be used as a
 * GDestroyNotify without casting
 */
static void
aggregate_event_free (gpointer e_)
{
  AggregateEvent *e = e_;

  g_clear_pointer (&e->aggregate_key, g_variant_unref);
  g_clear_pointer (&e->payload, g_variant_unref);
  g_clear_pointer (&e->date, g_free);

  g_free (e);
}

struct Fixture
{
  EmerAggregateTally *tally;
  GFile *tally_folder;
};

/**
 * Returns: a non-floating GVariant of type 'v', containing the given string.
 */
static GVariant *
v_str (const gchar *str)
{
  return g_variant_ref_sink (g_variant_new_variant (g_variant_new_string (str)));
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
  g_autoptr(GDateTime) datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) aggregate_key = v_str (G_STRFUNC);
  GVariant *payload = aggregate_key;

  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    1001,
                                    uuids[0],
                                    aggregate_key,
                                    payload,
                                    1,
                                    datetime,
                                    &error);
  g_assert_no_error (error);

  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    1001,
                                    uuids[0],
                                    aggregate_key,
                                    payload,
                                    2,
                                    datetime,
                                    &error);
  g_assert_no_error (error);
}

static EmerTallyIterResult
tally_iter_func (guint32     unix_user_id,
                 uuid_t      event_id,
                 GVariant   *aggregate_key,
                 GVariant   *payload,
                 guint32     counter,
                 const char *date,
                 gpointer    user_data)
{
  GPtrArray *events = user_data;

  g_ptr_array_add (events, aggregate_event_new (unix_user_id, event_id, aggregate_key, payload, counter, date));

  return EMER_TALLY_ITER_CONTINUE;
}

static void
test_aggregate_tally_iter (struct Fixture *fixture,
                           gconstpointer   payload_str)
{
  g_autoptr(GDateTime) datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  g_autoptr(GVariant) aggregate_key = v_str (G_STRFUNC);
  g_autoptr(GVariant) payload = payload_str ? v_str (payload_str) : NULL;
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func (aggregate_event_free);
  int i;

  // Add the same aggregate event multiple times. It must
  // result in a single aggregate event with the sum of
  // counters
  for (i = 0; i < 10; i++)
    {
      g_autoptr(GError) error = NULL;

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_DAILY_EVENTS,
                                        1001,
                                        uuids[0],
                                        aggregate_key,
                                        payload,
                                        1,
                                        datetime,
                                        &error);

      g_assert_no_error (error);
    }

  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             events);

  g_assert_cmpuint (events->len, ==, 1);
  AggregateEvent *e = g_ptr_array_index (events, 0);
  g_assert_cmpuint (e->counter, ==, 10);

  g_ptr_array_set_size (events, 0);
  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             events);
  g_assert_cmpuint (events->len, ==, 0);
}

static void
test_aggregate_tally_large_counter_single (struct Fixture *fixture,
                                           gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  g_autoptr(GVariant) v = v_str (G_STRFUNC);
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func (aggregate_event_free);
  g_autoptr(GError) error = NULL;

  // Add an aggregate event with a counter too large to fit into a 32-bit
  // signed integer.
  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    1001,
                                    uuids[0],
                                    v,
                                    v,
                                    ((guint32) G_MAXINT32) + 1,
                                    datetime,
                                    &error);
  g_assert_no_error (error);

  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             events);

  g_assert_cmpuint (events->len, ==, 1);
  AggregateEvent *e = g_ptr_array_index (events, 0);
  g_assert_cmpuint (e->counter, ==, ((guint32) G_MAXINT32) + 1);
}

static void
test_aggregate_tally_large_counter_add (struct Fixture *fixture,
                                        gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  g_autoptr(GVariant) v = v_str (G_STRFUNC);
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func (aggregate_event_free);
  g_autoptr(GError) error = NULL;

  // Add an aggregate event whose counter only just fits in a 32-bit signed
  // integer
  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    1001,
                                    uuids[0],
                                    v,
                                    v,
                                    (guint32) G_MAXINT32,
                                    datetime,
                                    &error);
  g_assert_no_error (error);

  // Now add 1 to it
  emer_aggregate_tally_store_event (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    1001,
                                    uuids[0],
                                    v,
                                    v,
                                    1,
                                    datetime,
                                    &error);
  g_assert_no_error (error);

  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             events);

  g_assert_cmpuint (events->len, ==, 1);
  AggregateEvent *e = g_ptr_array_index (events, 0);
  g_assert_cmpuint (e->counter, ==, ((guint32) G_MAXINT32) + 1);
}

static void
test_aggregate_tally_large_counter_upper_bound (struct Fixture *fixture,
                                                gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  g_autoptr(GVariant) v = v_str (G_STRFUNC);
  size_t i;
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func (aggregate_event_free);
  g_autoptr(GError) error = NULL;

  // The upper bound of an event's timer is 2 ** 32 - 1. Check that trying to
  // count above this limit is clamped at the limit.
  for (i = 0; i < 10; i++)
    {

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_DAILY_EVENTS,
                                        1001,
                                        uuids[0],
                                        v,
                                        v,
                                        G_MAXUINT32,
                                        datetime,
                                        &error);
      g_assert_no_error (error);
    }

  emer_aggregate_tally_iter (fixture->tally,
                             EMER_TALLY_DAILY_EVENTS,
                             datetime,
                             EMER_TALLY_ITER_FLAG_DELETE,
                             tally_iter_func,
                             events);

  g_assert_cmpuint (events->len, ==, 1);
  AggregateEvent *e = g_ptr_array_index (events, 0);
  g_assert_cmpuint (e->counter, ==, G_MAXUINT32);
}

static void
test_aggregate_tally_iter_before_daily (struct Fixture *fixture,
                                        gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  g_autoptr(GVariant) v = v_str (G_STRFUNC);
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func (aggregate_event_free);
  size_t i;
  g_autoptr(GError) error = NULL;

  // Add the same aggregate event to different days in the past and future
  for (i = 0; i < 50; i++)
    {
      g_autoptr(GDateTime) dt = g_date_time_add_days (datetime, i - 25);

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_DAILY_EVENTS,
                                        1001,
                                        uuids[0],
                                        v,
                                        v,
                                        1,
                                        dt,
                                        &error);

      g_assert_no_error (error);
    }

  /* Iterate but don't delete */
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    events);

  g_assert_cmpuint (events->len, ==, 25);
  for (i = 0; i < events->len; i++)
    {
      AggregateEvent *e = g_ptr_array_index (events, i);

      g_assert_cmpuint (e->counter, ==, 1);
      /* All returned dates should be before the requested date */
      g_assert_cmpstr (e->date, <, "2021-09-22");
    }

  /* Iterate again, and delete entries this time */
  g_ptr_array_set_size (events, 0);
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DELETE,
                                    tally_iter_func,
                                    events);

  g_assert_cmpuint (events->len, ==, 25);
  for (i = 0; i < events->len; i++)
    {
      AggregateEvent *e = g_ptr_array_index (events, i);

      g_assert_cmpuint (e->counter, ==, 1);
    }

  /* There should be nothing else to iterate now */
  g_ptr_array_set_size (events, 0);
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    events);

  g_assert_cmpuint (events->len, ==, 0);
}

static void
test_aggregate_tally_iter_before_monthly (struct Fixture *fixture,
                                          gconstpointer   dontuseme)
{
  g_autoptr(GDateTime) datetime = g_date_time_new_utc (2021, 9, 22, 0, 0, 0);
  g_autoptr(GVariant) v = v_str (G_STRFUNC);
  g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func (aggregate_event_free);
  size_t i;
  g_autoptr(GError) error = NULL;

  // Add the same event in different months of the past and future
  for (i = 0; i < 24; i++)
    {
      g_autoptr(GDateTime) dt = g_date_time_add_months (datetime, i - 12);

      emer_aggregate_tally_store_event (fixture->tally,
                                        EMER_TALLY_MONTHLY_EVENTS,
                                        1001,
                                        uuids[0],
                                        v,
                                        v,
                                        1,
                                        dt,
                                        &error);

      g_assert_no_error (error);
    }

  /* Iterate but don't delete */
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    events);

  g_assert_cmpuint (events->len, ==, 12);
  for (i = 0; i < events->len; i++)
    {
      AggregateEvent *e = g_ptr_array_index (events, i);

      g_assert_cmpuint (e->counter, ==, 1);
      /* All returned dates should be before the requested date */
      g_assert_cmpstr (e->date, <, "2021-09");
    }

  /* Iterate again, and delete entries this time */
  g_ptr_array_set_size (events, 0);
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DELETE,
                                    tally_iter_func,
                                    events);

  g_assert_cmpuint (events->len, ==, 12);
  for (i = 0; i < events->len; i++)
    {
      AggregateEvent *e = g_ptr_array_index (events, i);

      g_assert_cmpuint (e->counter, ==, 1);
    }

  /* There should be nothing else iterate now */
  g_ptr_array_set_size (events, 0);
  emer_aggregate_tally_iter_before (fixture->tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    datetime,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    tally_iter_func,
                                    events);

  g_assert_cmpuint (events->len, ==, 0);
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  for (size_t i = 0; i < G_N_ELEMENTS (uuid_strs); i++)
    g_assert_cmpint (0, ==, uuid_parse (uuid_strs[i], uuids[i]));

  g_test_init (&argc, (gchar ***) &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

#define ADD_AGGREGATE_TALLY_TEST_FUNC(path, func) \
  g_test_add((path), struct Fixture, NULL, setup, (func), teardown)

  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/new-succeeds",
                                 test_aggregate_tally_new_succeeds);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/store-events",
                                 test_aggregate_tally_store_events);
  g_test_add ("/aggregate-tally/iter/null-payload",
              struct Fixture,
              NULL,
              setup,
              test_aggregate_tally_iter,
              teardown);
  g_test_add ("/aggregate-tally/iter/nonnull-payload",
              struct Fixture,
              "what a big payload you have, grandma",
              setup,
              test_aggregate_tally_iter,
              teardown);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/large-counter/single",
                                 test_aggregate_tally_large_counter_single);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/large-counter/add",
                                 test_aggregate_tally_large_counter_add);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/large-counter/upper-bound",
                                 test_aggregate_tally_large_counter_upper_bound);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/iter-before/daily",
                                 test_aggregate_tally_iter_before_daily);
  ADD_AGGREGATE_TALLY_TEST_FUNC ("/aggregate-tally/iter-before/monthly",
                                 test_aggregate_tally_iter_before_monthly);

#undef ADD_AGGREGATE_TALLY_TEST_FUNC


  return g_test_run ();
}
