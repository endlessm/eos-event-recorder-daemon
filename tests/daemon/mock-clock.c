#include "mock-clock.h"

typedef enum _MockClocks {
  MOCK_CLOCK_TYPE_MONOTONIC = 0
} MockClockType;

struct _MockClock
{
  GObject parent_instance;

  /* Microseconds, indexed by MockClockType */
  gint64 times[1];
};

typedef struct _MockClockSource {
  GSource parent;

  MockClock *clock;
  MockClockType type_;

  /* Microseconds */
  guint interval;
  gint64 next_ready_time;
} MockClockSource;

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

static gboolean
mock_clock_source_check (GSource *source)
{
  MockClockSource *self = (MockClockSource *) source;

  return self->clock->times[self->type_] >= self->next_ready_time;
}

static gboolean
mock_clock_source_dispatch (GSource    *source,
                           GSourceFunc callback,
                           gpointer    user_data)
{
  MockClockSource *self = (MockClockSource *) source;

  g_return_val_if_fail (callback != NULL, G_SOURCE_REMOVE);

  self->next_ready_time += self->interval;
  return callback (user_data);
}

static void
mock_clock_source_finalize (GSource *source)
{
  MockClockSource *self = (MockClockSource *) source;

  g_clear_object (&self->clock);
}

static GSourceFuncs mock_clock_source_funcs = {
  .check = mock_clock_source_check,
  .dispatch = mock_clock_source_dispatch,
  .finalize = mock_clock_source_finalize,
};

static GSource *
mock_clock_source_new (MockClock *self,
                       MockClockType type_,
                       guint interval)
{
  g_return_val_if_fail (MOCK_IS_CLOCK (self), NULL);

  MockClockSource *source =
      (MockClockSource *) g_source_new (&mock_clock_source_funcs,
                                        sizeof (MockClockSource));
  source->clock = g_object_ref (self);
  source->type_ = type_;
  source->interval = interval;
  source->next_ready_time = self->times[type_] + interval;

  return (GSource *) source;
}

static GSource *
mock_clock_timeout_source_new_seconds (EmerClock *self,
                                       guint      interval)
{
  g_return_val_if_fail (MOCK_IS_CLOCK (self), NULL);

  return mock_clock_source_new (MOCK_CLOCK (self),
                                MOCK_CLOCK_TYPE_MONOTONIC,
                                interval);
}

static void
clock_iface_init (EmerClockInterface *iface,
                  gpointer            iface_data)
{
  iface->timeout_source_new_seconds = mock_clock_timeout_source_new_seconds;
}

static void
mock_clock_init (MockClock *self)
{
  self->times[MOCK_CLOCK_TYPE_MONOTONIC] = 0x1;
}

void
mock_clock_advance_monotonic (MockClock *self,
                              gint64     delta_usecs)
{
  g_return_if_fail (delta_usecs >= 0);
  g_return_if_fail (self->times[MOCK_CLOCK_TYPE_MONOTONIC] <= G_MAXINT64 - delta_usecs);

  self->times[MOCK_CLOCK_TYPE_MONOTONIC] += delta_usecs;
}
