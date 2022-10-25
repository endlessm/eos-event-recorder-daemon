#pragma once

#include <glib-object.h>
#include "shared/emer-clock.h"

G_BEGIN_DECLS

#define EMER_TYPE_REAL_CLOCK (emer_real_clock_get_type())

G_DECLARE_FINAL_TYPE (EmerRealClock, emer_real_clock, EMER, REAL_CLOCK, GObject)

EmerClock *emer_real_clock_new (void);

G_END_DECLS
