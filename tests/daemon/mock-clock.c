#include "mock-clock.h"

struct _MockClock
{
  GObject parent_instance;
};

static void clock_iface_init (EmerClockInterface *iface,
                              gpointer            iface_data);

G_DEFINE_FINAL_TYPE_WITH_CODE (MockClock, mock_clock, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (EMER_TYPE_CLOCK, clock_iface_init))

MockClock *
mock_clock_new (void)
{
  return g_object_new (MOCK_TYPE_CLOCK, NULL);
}

static void
mock_clock_class_init (MockClockClass *klass)
{
}

static void
clock_iface_init (EmerClockInterface *iface,
                  gpointer            iface_data)
{
}

static void
mock_clock_init (MockClock *self)
{
}
