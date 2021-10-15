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

#include "emer-aggregate-timer-impl.h"
#include "emer-aggregate-tally.h"

struct _EmerAggregateTimerImpl
{
  GObject parent_instance;

  EmerAggregateTimer *timer; /* owned */
  EmerAggregateTally *tally; /* unowned */
  gint64 start_monotonic_us;
  gint64 end_monotonic_us;

  guint32 unix_user_id;
  GVariant *event_id; /* owned */
  GVariant *aggregate_key; /* owned */
  GVariant *payload; /* owned */
  gchar *cache_key_string; /* owned */
  gchar *sender_name; /* owned */
};

G_DEFINE_TYPE (EmerAggregateTimerImpl, emer_aggregate_timer_impl, G_TYPE_OBJECT)

static void
emer_aggregate_timer_impl_finalize (GObject *object)
{
  EmerAggregateTimerImpl *self = (EmerAggregateTimerImpl *)object;

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self->timer));

  g_clear_pointer (&self->event_id, g_variant_unref);
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
  g_autoptr(GVariant) cache_key = NULL;

  g_variant_take_ref (event_id);
  g_variant_take_ref (aggregate_key);
  if (payload)
    g_variant_take_ref (payload);

  cache_key = g_variant_new ("(u@ayvmv)",
                             unix_user_id,
                             event_id,
                             aggregate_key,
                             payload);

  self = g_object_new (EMER_TYPE_AGGREGATE_TIMER_IMPL, NULL);
  self->timer = timer;
  self->sender_name = g_strdup (sender_name);
  self->tally = tally;
  self->unix_user_id = unix_user_id;
  self->event_id = g_variant_ref (event_id);
  self->aggregate_key = g_variant_ref (aggregate_key);
  self->payload = payload ? g_variant_ref (payload) : NULL;
  self->start_monotonic_us = monotonic_time_us;
  self->cache_key_string = g_variant_print (cache_key, TRUE);

  return self;
}

gboolean
emer_aggregate_timer_impl_store (EmerAggregateTimerImpl  *self,
                                 const gchar             *date,
                                 gint64                   monotonic_time_us,
                                 GError                 **error)
{
  gint64 counter;

  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), FALSE);
  g_return_val_if_fail (date != NULL && *date != '\0', FALSE);

  counter = monotonic_time_us - self->start_monotonic_us;
  return emer_aggregate_tally_store_event (self->tally,
                                           self->unix_user_id,
                                           self->event_id,
                                           self->aggregate_key,
                                           self->payload,
                                           counter,
                                           date,
                                           monotonic_time_us,
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
  g_autofree gchar *month_date = NULL;
  g_autofree gchar *date = NULL;
  gint64 counter;

  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), FALSE);

  counter = monotonic_time_us - self->start_monotonic_us;

  date = g_date_time_format (datetime, "%Y-%m-%d");
  emer_aggregate_tally_store_event (self->tally,
                                    self->unix_user_id,
                                    self->event_id,
                                    self->aggregate_key,
                                    self->payload,
                                    counter,
                                    date,
                                    monotonic_time_us,
                                    &local_error);
  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }


  month_date = g_date_time_format (datetime, "%Y-%m");
  emer_aggregate_tally_store_event (self->tally,
                                    self->unix_user_id,
                                    self->event_id,
                                    self->aggregate_key,
                                    self->payload,
                                    counter,
                                    month_date,
                                    monotonic_time_us,
                                    &local_error);
  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  return TRUE;
}

const gchar *
emer_aggregate_timer_impl_get_sender_name (EmerAggregateTimerImpl *self)
{
  g_return_val_if_fail (EMER_IS_AGGREGATE_TIMER_IMPL (self), NULL);

  return self->sender_name;
}

guint
emer_aggregate_timer_impl_hash (gconstpointer timer_impl)
{
  const EmerAggregateTimerImpl *self = EMER_AGGREGATE_TIMER_IMPL ((gpointer)timer_impl);

  return g_str_hash (self->cache_key_string);
}

gboolean
emer_aggregate_timer_impl_equal (gconstpointer a,
                                 gconstpointer b)
{
  const EmerAggregateTimerImpl *timer_impl_a = EMER_AGGREGATE_TIMER_IMPL ((gpointer)a);
  const EmerAggregateTimerImpl *timer_impl_b = EMER_AGGREGATE_TIMER_IMPL ((gpointer)b);

  return g_str_equal (timer_impl_a->cache_key_string,
                      timer_impl_b->cache_key_string);
}
