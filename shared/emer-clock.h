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
