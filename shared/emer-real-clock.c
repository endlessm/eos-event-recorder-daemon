#include "emer-real-clock.h"

struct _EmerRealClock
{
  GObject parent_instance;
};

static void clock_iface_init (EmerClockInterface *iface,
                              gpointer            iface_data);

G_DEFINE_FINAL_TYPE_WITH_CODE (EmerRealClock, emer_real_clock, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (EMER_TYPE_CLOCK, clock_iface_init))

EmerClock *
emer_real_clock_new (void)
{
  return g_object_new (EMER_TYPE_REAL_CLOCK, NULL);
}

static void
emer_real_clock_class_init (EmerRealClockClass *klass)
{
}

static void
clock_iface_init (EmerClockInterface *iface,
                  gpointer            iface_data)
{
}

static void
emer_real_clock_init (EmerRealClock *self)
{
}
