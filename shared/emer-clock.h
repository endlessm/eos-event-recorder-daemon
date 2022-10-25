#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_CLOCK (emer_clock_get_type ())

G_DECLARE_INTERFACE (EmerClock, emer_clock, EMER, CLOCK, GObject)

struct _EmerClockInterface
{
  GTypeInterface parent;
};

G_END_DECLS
