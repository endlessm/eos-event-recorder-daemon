/* emer-real-clock.h
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
#include "shared/emer-clock.h"

G_BEGIN_DECLS

#define EMER_TYPE_REAL_CLOCK (emer_real_clock_get_type())

G_DECLARE_FINAL_TYPE (EmerRealClock, emer_real_clock, EMER, REAL_CLOCK, GObject)

EmerClock *emer_real_clock_new (void);

G_END_DECLS
