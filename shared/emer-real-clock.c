/* emer-real-clock.c
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
#include "emer-real-clock.h"

struct _EmerRealClock
{
  GObject parent_instance;
};

static void clock_iface_init (EmerClockInterface *iface,
                              gpointer            iface_data);

G_DEFINE_FINAL_TYPE_WITH_CODE (EmerRealClock, emer_real_clock, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (EMER_TYPE_CLOCK, clock_iface_init))

EmerClock *
emer_real_clock_new (void)
{
  return g_object_new (EMER_TYPE_REAL_CLOCK, NULL);
}

static void
emer_real_clock_class_init (EmerRealClockClass *klass)
{
}

static GSource *
emer_real_clock_timeout_source_new_seconds (EmerClock *self,
                                            guint      interval)
{
  return g_timeout_source_new_seconds (interval);
}

static void
clock_iface_init (EmerClockInterface *iface,
                  gpointer            iface_data)
{
  iface->timeout_source_new_seconds = emer_real_clock_timeout_source_new_seconds;
}

static void
emer_real_clock_init (EmerRealClock *self)
{
}
