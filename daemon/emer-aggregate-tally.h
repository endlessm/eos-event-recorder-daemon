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
#include <uuid.h>

G_BEGIN_DECLS

typedef enum {
  EMER_TALLY_ITER_FLAG_DEFAULT = 0,
  EMER_TALLY_ITER_FLAG_DELETE = 1 << 0,
} EmerTallyIterFlags;

typedef enum {
  EMER_TALLY_ITER_CONTINUE = 0,
  EMER_TALLY_ITER_STOP = 1,
} EmerTallyIterResult;

typedef enum
{
  EMER_TALLY_DAILY_EVENTS,
  EMER_TALLY_MONTHLY_EVENTS,
} EmerTallyType;

typedef EmerTallyIterResult (*EmerTallyIterFunc) (guint32     unix_user_id,
                                                  uuid_t      event_id,
                                                  GVariant   *aggregate_key,
                                                  GVariant   *payload,
                                                  guint32     counter,
                                                  const char *date,
                                                  gpointer    user_data);

#define EMER_TYPE_AGGREGATE_TALLY (emer_aggregate_tally_get_type())
G_DECLARE_FINAL_TYPE (EmerAggregateTally,
                      emer_aggregate_tally,
                      EMER, AGGREGATE_TALLY, GObject)

EmerAggregateTally *
emer_aggregate_tally_new (const gchar *persistent_cache_directory);

gboolean emer_aggregate_tally_store_event (EmerAggregateTally  *self,
                                           EmerTallyType        tally_type,
                                           guint32              unix_user_id,
                                           uuid_t               event_id,
                                           GVariant            *aggregate_key,
                                           GVariant            *payload,
                                           guint32              counter,
                                           GDateTime            *datetime,
                                           GError             **error);

void emer_aggregate_tally_iter (EmerAggregateTally *self,
                                EmerTallyType       tally_type,
                                GDateTime          *datetime,
                                EmerTallyIterFlags  flags,
                                EmerTallyIterFunc   func,
                                gpointer            user_data);

void emer_aggregate_tally_iter_before (EmerAggregateTally *self,
                                       EmerTallyType       tally_type,
                                       GDateTime          *datetime,
                                       EmerTallyIterFlags  flags,
                                       EmerTallyIterFunc   func,
                                       gpointer            user_data);

G_END_DECLS
