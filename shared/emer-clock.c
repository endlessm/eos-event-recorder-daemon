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
