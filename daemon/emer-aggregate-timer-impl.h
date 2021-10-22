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

#pragma once

#include <glib-object.h>

#include "emer-aggregate-tally.h"
#include "emer-event-recorder-server.h"

G_BEGIN_DECLS

#define EMER_TYPE_AGGREGATE_TIMER_IMPL (emer_aggregate_timer_impl_get_type())
G_DECLARE_FINAL_TYPE (EmerAggregateTimerImpl,
                      emer_aggregate_timer_impl,
                      EMER, AGGREGATE_TIMER_IMPL, GObject)

EmerAggregateTimerImpl *
emer_aggregate_timer_impl_new (EmerAggregateTally *tally,
                               EmerAggregateTimer *timer,
                               const gchar        *sender_name,
                               guint32             unix_user_id,
                               GVariant           *event_id,
                               GVariant           *aggregate_key,
                               GVariant           *payload,
                               gint64              monotonic_time_us);

gboolean emer_aggregate_timer_impl_store (EmerAggregateTimerImpl  *self,
                                          const gchar             *date,
                                          gint64                   monotonic_time_us,
                                          GError                 **error);

void emer_aggregate_timer_impl_split (EmerAggregateTimerImpl *self,
                                      gint64                  monotonic_time_us);

gboolean emer_aggregate_timer_impl_stop (EmerAggregateTimerImpl  *self,
                                         GDateTime               *datetime,
                                         gint64                   monotonic_time_us,
                                         GError                 **error);

const gchar *
emer_aggregate_timer_impl_get_sender_name (EmerAggregateTimerImpl *self);

guint emer_aggregate_timer_impl_hash (gconstpointer timer_impl);
gboolean emer_aggregate_timer_impl_equal (gconstpointer a,
                                          gconstpointer b);

G_END_DECLS
