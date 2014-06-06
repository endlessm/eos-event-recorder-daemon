/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L /* for clock_gettime() */
#endif

#include "metrics-util.h"

#include <errno.h>
#include <time.h>

#include <glib.h>
#include <gio/gio.h>
#include <uuid/uuid.h>

void
get_uuid_builder (uuid_t           uuid,
                  GVariantBuilder *uuid_builder)
{
  g_variant_builder_init (uuid_builder, G_VARIANT_TYPE ("ay"));
  for (size_t i = 0; i < UUID_LENGTH; ++i)
    g_variant_builder_add (uuid_builder, "y", uuid[i]);
}

gboolean
get_current_time (clockid_t clock_id,
                  gint64   *current_time)
{
  g_return_val_if_fail (current_time != NULL, FALSE);

  // Get the time before doing anything else because it will change during
  // execution.
  struct timespec ts;
  int gettime_failed = clock_gettime (clock_id, &ts);
  if (gettime_failed != 0)
    {
      int error_code = errno;
      g_critical ("Attempt to get current time failed: %s",
                  g_strerror (error_code));
      return FALSE;
    }

  // Ensure that the clock provides a time that can be safely represented in a
  // gint64 in nanoseconds.
  g_assert (ts.tv_sec >= (G_MININT64 / NANOSECONDS_PER_SECOND));
  g_assert (ts.tv_sec <= (G_MAXINT64 / NANOSECONDS_PER_SECOND));
  g_assert (ts.tv_nsec >= 0);
  g_assert (ts.tv_nsec < NANOSECONDS_PER_SECOND);

  // We already know that ts.tv_sec <= (G_MAXINT64 / NANOSECONDS_PER_SECOND).
  // This handles the edge case where
  // ts.tv_sec == (G_MAXINT64 / NANOSECONDS_PER_SECOND).
  g_assert ((ts.tv_sec < (G_MAXINT64 / NANOSECONDS_PER_SECOND)) ||
            (ts.tv_nsec <= (G_MAXINT64 % NANOSECONDS_PER_SECOND)));

  *current_time = (NANOSECONDS_PER_SECOND * ((gint64) ts.tv_sec))
    + ((gint64) ts.tv_nsec);
  return TRUE;
}
