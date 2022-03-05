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
#include "emer-aggregate-timer-impl.h"
#include "emer-aggregate-tally.h"
#include "shared/metrics-util.h"

#include <uuid/uuid.h>

struct _EmerAggregateTimerImpl
{
  GObject parent_instance;

  EmerAggregateTimer *timer; /* owned */
  EmerAggregateTally *tally; /* unowned */
  gint64 start_monotonic_us;

  guint32 unix_user_id;
  uuid_t event_id;
  uuid_t monthly_event_id;
  GVariant *aggregate_key; /* owned */
  GVariant *payload; /* owned */
  gchar *cache_key_string; /* owned */
  gchar *sender_name; /* owned */

  guint32 run_count;
};

G_DEFINE_TYPE (EmerAggregateTimerImpl, emer_aggregate_timer_impl, G_TYPE_OBJECT)

static void
emer_aggregate_timer_impl_finalize (GObject *object)
{
  EmerAggregateTimerImpl *self = (EmerAggregateTimerImpl *)object;

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->timer));

  g_clear_pointer (&self->aggregate_key, g_variant_unref);
  g_clear_pointer (&self->payload, g_variant_unref);
  g_clear_pointer (&self->cache_key_string, g_free);
  g_clear_pointer (&self->sender_name, g_free);
  g_clear_object (&self->timer);

  G_OBJECT_CLASS (emer_aggregate_timer_impl_parent_class)->finalize (object);
}

static void
emer_aggregate_timer_impl_class_init (EmerAggregateTimerImplClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = emer_aggregate_timer_impl_finalize;
}

static void
emer_aggregate_timer_impl_init (EmerAggregateTimerImpl *self)
{
}

EmerAggregateTimerImpl *
emer_aggregate_timer_impl_new (EmerAggregateTally *tally,
                               EmerAggregateTimer *timer,
                               const gchar        *sender_name,
                               guint32             unix_user_id,
                               GVariant           *event_id,
                               GVariant           *aggregate_key,
                               GVariant           *payload,
                               gint64              monotonic_time_us)
{
  EmerAggregateTimerImpl *self;
  gconstpointer event_id_bytes = NULL;
  gsize event_id_len = 0;
  uuid_t event_uuid;

  /* TODO: factor this out */
  g_return_val_if_fail (g_variant_is_of_type (event_id, G_VARIANT_TYPE_BYTESTRING), NULL);
  g_return_val_if_fail (g_variant_is_of_type (aggregate_key, G_VARIANT_TYPE_VARIANT), NULL);
  g_return_val_if_fail (payload == NULL || g_variant_is_of_type (payload, G_VARIANT_TYPE_VARIANT), NULL);

  event_id_bytes = g_variant_get_fixed_array (event_id, &event_id_len, sizeof (event_uuid[0]));
  g_return_val_if_fail (event_id_len == sizeof (event_uuid), NULL);

  g_variant_take_ref (aggregate_key);
  if (payload)
    g_variant_take_ref (payload);

  self = g_object_new (EMER_TYPE_AGGREGATE_TIMER_IMPL, NULL);
  self->timer = timer;
  self->sender_name = g_strdup (sender_name);
  self->tally = tally;
  self->unix_user_id = unix_user_id;

  memcpy (self->event_id, event_id_bytes, event_id_len);
  uuid_generate_sha1 (self->monthly_event_id, self->event_id, "monthly", strlen ("monthly"));

  self->aggregate_key = g_variant_ref (aggregate_key);
  self->payload = payload ? g_variant_ref (payload) : NULL;
  self->start_monotonic_us = monotonic_time_us;
  self->cache_key_string =
    emer_aggregate_timer_impl_compose_hash_string (sender_name,
                                                   unix_user_id,
                                                   event_id,
                                                   aggregate_key,
                                                   payload);
  self->run_count = 1;

  return self;
}

gboolean
emer_aggregate_timer_impl_store (EmerAggregateTimerImpl  *self,
                                 EmerTallyType            tally_type,
                                 GDateTime               *datetime,
                                 gint64                   monotonic_time_us,
                                 GError                 **error)
{
  uuid_t *event_id;
  guint32 counter;
  gint64 difference;

  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), FALSE);
  g_return_val_if_fail (datetime != NULL, FALSE);

  difference = monotonic_time_us - self->start_monotonic_us;
  counter = CLAMP (difference / G_USEC_PER_SEC, 0, G_MAXUINT32);

  switch (tally_type)
    {
    case EMER_TALLY_DAILY_EVENTS:
      event_id = &self->event_id;
      break;

    case EMER_TALLY_MONTHLY_EVENTS:
      event_id = &self->monthly_event_id;
      break;

    default:
      g_assert_not_reached ();
    }

  return emer_aggregate_tally_store_event (self->tally,
                                           tally_type,
                                           self->unix_user_id,
                                           *event_id,
                                           self->payload,
                                           counter,
                                           datetime,
                                           error);
}

void
emer_aggregate_timer_impl_split (EmerAggregateTimerImpl *self,
                                 gint64                  monotonic_time_us)
{
  g_return_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self));

  self->start_monotonic_us = monotonic_time_us;
}

gboolean
emer_aggregate_timer_impl_stop (EmerAggregateTimerImpl  *self,
                                GDateTime               *datetime,
                                gint64                   monotonic_time_us,
                                GError                 **error)
{
  g_autoptr(GError) local_error = NULL;
  guint32 counter;
  gint64 difference;

  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), FALSE);

  difference = monotonic_time_us - self->start_monotonic_us;
  counter = CLAMP (difference / G_USEC_PER_SEC, 0, G_MAXUINT32);

  emer_aggregate_tally_store_event (self->tally,
                                    EMER_TALLY_DAILY_EVENTS,
                                    self->unix_user_id,
                                    self->event_id,
                                    self->payload,
                                    counter,
                                    datetime,
                                    &local_error);
  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }


  emer_aggregate_tally_store_event (self->tally,
                                    EMER_TALLY_MONTHLY_EVENTS,
                                    self->unix_user_id,
                                    self->monthly_event_id,
                                    self->payload,
                                    counter,
                                    datetime,
                                    &local_error);
  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

const gchar *
emer_aggregate_timer_impl_get_cache_key (EmerAggregateTimerImpl *self)
{
  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), NULL);

  return self->cache_key_string;
}

const gchar *
emer_aggregate_timer_impl_get_sender_name (EmerAggregateTimerImpl *self)
{
  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), NULL);

  return self->sender_name;
}

gchar *
emer_aggregate_timer_impl_compose_hash_string (const gchar *sender_name,
                                               guint32      unix_user_id,
                                               GVariant    *event_id,
                                               GVariant    *aggregate_key,
                                               GVariant    *payload)
{
  g_autoptr(GVariant) cache_key = NULL;

  cache_key = g_variant_new ("(su@ayvmv)",
                             sender_name,
                             unix_user_id,
                             g_variant_take_ref (event_id),
                             g_variant_take_ref (aggregate_key),
                             payload ? g_variant_take_ref (payload) : NULL);

  return g_variant_print (cache_key, TRUE);
}

void
emer_aggregate_timer_impl_push_run_count (EmerAggregateTimerImpl *self)
{
  g_return_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self));

  self->run_count++;
}

gboolean
emer_aggregate_timer_impl_pop_run_count (EmerAggregateTimerImpl *self)
{
  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), FALSE);

  self->run_count--;

  return self->run_count == 0;
}
