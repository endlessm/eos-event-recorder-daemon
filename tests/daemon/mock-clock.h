#pragma once

#include "shared/emer-clock.h"

G_BEGIN_DECLS

#define MOCK_TYPE_CLOCK (mock_clock_get_type())

G_DECLARE_FINAL_TYPE (MockClock, mock_clock, MOCK, CLOCK, GObject)

MockClock *mock_clock_new (void);

void mock_clock_advance_monotonic (MockClock *self,
                                   gint64     delta_usecs);

G_END_DECLS
