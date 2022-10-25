/* emer-clock.c
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
#include "emer-clock.h"

G_DEFINE_INTERFACE (EmerClock, emer_clock, G_TYPE_OBJECT)

static void
emer_clock_default_init (EmerClockInterface *iface)
{

}

guint
emer_clock_timeout_add_seconds (EmerClock  *self,
                                guint       interval,
                                GSourceFunc function,
                                gpointer    data)
{
  g_return_val_if_fail (EMER_IS_CLOCK (self), 0);

  EmerClockInterface *iface = EMER_CLOCK_GET_IFACE (self);

  g_return_val_if_fail (iface->timeout_source_new_seconds != NULL, 0);

  g_autoptr(GSource) source = iface->timeout_source_new_seconds (self, interval);
  g_source_set_callback (source, function, data, NULL);
  return g_source_attach (source, NULL);
}
