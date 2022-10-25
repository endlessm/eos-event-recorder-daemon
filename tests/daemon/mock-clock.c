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

struct _MockClock
{
  GObject parent_instance;
};

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

static void
clock_iface_init (EmerClockInterface *iface,
                  gpointer            iface_data)
{
}

static void
mock_clock_init (MockClock *self)
{
}
