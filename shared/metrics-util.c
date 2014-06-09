/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "metrics-util.h"

/* For clock_gettime() */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 199309L
#error "This code requires _POSIX_C_SOURCE to be 199309L or later."
#endif

#include <errno.h>
#include <time.h>
#include <inttypes.h>

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
  if (ts.tv_sec < G_MININT64 / NANOSECONDS_PER_SECOND ||
      ts.tv_sec > G_MAXINT64 / NANOSECONDS_PER_SECOND ||
      ts.tv_nsec < 0 ||
      ts.tv_nsec >= NANOSECONDS_PER_SECOND ||
      // We already know that ts.tv_sec <= G_MAXINT64 / NANOSECONDS_PER_SECOND.
      // This handles the edge case where
      // ts.tv_sec == G_MAXINT64 / NANOSECONDS_PER_SECOND.
      (ts.tv_sec == G_MAXINT64 / NANOSECONDS_PER_SECOND &&
       ts.tv_nsec > G_MAXINT64 % NANOSECONDS_PER_SECOND))
    {
      /* The (gint64) conversion is to handle the fact that time_t's size is
      platform-defined; so we cast it to 64 bits. tv_nsec is defined as long. */
      g_critical ("Clock returned a time that does not fit in a 64-bit integer "
                  "in nanoseconds (seconds %" PRId64 ", nanoseconds "
                  "%ld.)", (gint64) ts.tv_sec, ts.tv_nsec);
      return FALSE;
    }

  *current_time = (NANOSECONDS_PER_SECOND * ((gint64) ts.tv_sec))
    + ((gint64) ts.tv_nsec);
  return TRUE;
}
