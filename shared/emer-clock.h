/* emer-clock.h
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
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_CLOCK (emer_clock_get_type ())

G_DECLARE_INTERFACE (EmerClock, emer_clock, EMER, CLOCK, GObject)

struct _EmerClockInterface
{
  GTypeInterface parent;

  GSource *(*timeout_source_new_seconds) (EmerClock *self,
                                          guint      interval);
};

guint emer_clock_timeout_add_seconds (EmerClock  *self,
                                      guint       interval,
                                      GSourceFunc function,
                                      gpointer    data);

G_END_DECLS
