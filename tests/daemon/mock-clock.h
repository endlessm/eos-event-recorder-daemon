/* mock-clock.h
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

#include "shared/emer-clock.h"

G_BEGIN_DECLS

#define MOCK_TYPE_CLOCK (mock_clock_get_type())

G_DECLARE_FINAL_TYPE (MockClock, mock_clock, MOCK, CLOCK, GObject)

MockClock *mock_clock_new (void);

void mock_clock_advance_monotonic (MockClock *self,
                                   gint64     delta_us);

G_END_DECLS
