/* mock-clock.c
 *
 * Copyright 2022 Endless OS Foundation LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "mock-clock.h"

typedef enum {
  MOCK_CLOCK_TYPE_MONOTONIC = 0
} MockClockType;

struct _MockClock
{
  GObject parent_instance;

  /* Microseconds, indexed by MockClockType */
  gint64 times_us[1];
};

typedef struct {
  GSource parent;

  MockClock *clock; /* (not nullable) (owned) */
  MockClockType type_;

  /* Microseconds */
  guint64 interval_us;
  gint64 next_ready_time_us;
} MockClockSource;

static void clock_iface_init (EmerClockInterface *iface,
                              gpointer            iface_data);

G_DEFINE_FINAL_TYPE_WITH_CODE (MockClock, mock_clock, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (EMER_TYPE_CLOCK, clock_iface_init))

MockClock *
mock_clock_new (void)
{
  return g_object_new (MOCK_TYPE_CLOCK, NULL);
}

static void
mock_clock_class_init (MockClockClass *klass)
{
}

static gboolean
mock_clock_source_check (GSource *source)
{
  MockClockSource *self = (MockClockSource *) source;

  return self->clock->times_us[self->type_] >= self->next_ready_time_us;
}

static gboolean
mock_clock_source_dispatch (GSource    *source,
                            GSourceFunc callback,
                            gpointer    user_data)
{
  MockClockSource *self = (MockClockSource *) source;

  g_return_val_if_fail (callback != NULL, G_SOURCE_REMOVE);

  self->next_ready_time_us += self->interval_us;
  return callback (user_data);
}

static void
mock_clock_source_finalize (GSource *source)
{
  MockClockSource *self = (MockClockSource *) source;

  g_clear_object (&self->clock);
}

static GSourceFuncs mock_clock_source_funcs = {
  .check = mock_clock_source_check,
  .dispatch = mock_clock_source_dispatch,
  .finalize = mock_clock_source_finalize,
};

static GSource *
mock_clock_source_new (MockClock     *self,
                       MockClockType  type_,
                       guint          interval_us)
{
  g_return_val_if_fail (MOCK_IS_CLOCK (self), NULL);

  MockClockSource *source =
      (MockClockSource *) g_source_new (&mock_clock_source_funcs,
                                        sizeof (MockClockSource));
  source->clock = g_object_ref (self);
  source->type_ = type_;
  source->interval_us = interval_us;
  source->next_ready_time_us = self->times_us[type_] + interval_us;

  return (GSource *) source;
}

static GSource *
mock_clock_timeout_source_new_seconds (EmerClock *self,
                                       guint      interval)
{
  g_return_val_if_fail (MOCK_IS_CLOCK (self), NULL);

  return mock_clock_source_new (MOCK_CLOCK (self),
                                MOCK_CLOCK_TYPE_MONOTONIC,
                                interval * G_USEC_PER_SEC);
}

static void
clock_iface_init (EmerClockInterface *iface,
                  gpointer            iface_data)
{
  iface->timeout_source_new_seconds = mock_clock_timeout_source_new_seconds;
}

static void
mock_clock_init (MockClock *self)
{
}

void
mock_clock_advance_monotonic (MockClock *self,
                              gint64     delta_us)
{
  g_return_if_fail (delta_us >= 0);
  g_return_if_fail (self->times_us[MOCK_CLOCK_TYPE_MONOTONIC] <= G_MAXINT64 - delta_us);

  self->times_us[MOCK_CLOCK_TYPE_MONOTONIC] += delta_us;
}
