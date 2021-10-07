/* Copyright 2018–2021 Endless OS Foundation LLC */

/* This file is part of eos-metrics-instrumentation.
 *
 * eos-metrics-instrumentation is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 *
 * eos-metrics-instrumentation is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with eos-metrics-instrumentation.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "eins-boottime-source.h"

#include <errno.h>
#include <inttypes.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

typedef struct {
  GSource parent;

  int fd;
  gpointer tag;
} EinsBoottimeSource;

static gboolean
eins_boottime_check (GSource *source)
{
  EinsBoottimeSource *self = (EinsBoottimeSource *) source;

  return g_source_query_unix_fd (source, self->tag) != 0;
}

static gboolean
eins_boottime_source_dispatch (GSource    *source,
                               GSourceFunc callback,
                               gpointer    user_data)
{
  EinsBoottimeSource *self = (EinsBoottimeSource *) source;
  uint64_t n_expirations = 0;

  if (callback == NULL)
    {
      g_warning ("Boottime source dispatched without callback. "
                 "You must call g_source_set_callback().");
      return G_SOURCE_REMOVE;
    }

  /* Must read from the FD to reset its ready state. */
  if (read (self->fd, &n_expirations, sizeof (n_expirations)) < 0)
    {
      g_warning ("read() failed for timerfd: %s",
                 g_strerror (errno));
      return G_SOURCE_REMOVE;
    }

  return callback (user_data);
}

static void
eins_boottime_source_finalize (GSource *source)
{
  EinsBoottimeSource *self = (EinsBoottimeSource *) source;
  g_autoptr(GError) local_error = NULL;

  if (!g_close (self->fd, &local_error))
    g_warning ("Failed to close timerfd: %s", local_error->message);
  self->fd = -1;
}

static const GSourceFuncs eins_boottime_source_funcs = {
  .check = eins_boottime_check,
  .dispatch = eins_boottime_source_dispatch,
  .finalize = eins_boottime_source_finalize,
};

/**
 * eins_boottime_source_new_useconds:
 * @interval_us: the timeout interval, in microseconds
 * @error: return location for a #GError, or %NULL
 *
 * Like g_timeout_source_new(), but uses `CLOCK_BOOTTIME` to account for time
 * when the system is suspended.
 *
 * If @interval_us is set to zero, the #GSource will be ready the next time it's
 * checked.
 *
 * @error will be set to a #GIOError if, for example, the process runs out
 * of file descriptors.
 *
 * Returns: (transfer full): a new `CLOCK_BOOTTIME` #GSource, or %NULL with
 *   @error set
 */
static GSource *
eins_boottime_source_new_useconds (guint64   interval_us,
                                   GError  **error)
{
  g_autoptr(GSource) source = NULL;
  EinsBoottimeSource *self = NULL;
  int fd;
  struct itimerspec its = {
    .it_interval = {
      .tv_sec = interval_us / G_USEC_PER_SEC,
      .tv_nsec = (interval_us % G_USEC_PER_SEC) * 1000,
    }
  };

  its.it_value = its.it_interval;

  /* Set the GError if timerfd_create() fails because it could be e.g. ENFILE
   * which we should handle gracefully */
  fd = timerfd_create (CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK);
  if (G_UNLIKELY (fd < 0))
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "timerfd_create (CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK) failed: %s",
                   g_strerror (errno));
      return NULL;
    }

  /* But use g_error() if timerfd_settime() fails which likely would indicate
   * programmer error */
  /* FIXME: If this is upstreamed to GLib, handle EINVAL gracefully */
  if (G_UNLIKELY (timerfd_settime (fd,
                                   0,
                                   &its,
                                   NULL /* old_value */) < 0))
    g_error ("timerfd_settime() failed: %s",
             g_strerror (errno));

  source = g_source_new ((GSourceFuncs *)&eins_boottime_source_funcs,
                         sizeof (EinsBoottimeSource));
  self = (EinsBoottimeSource *) source;
  self->fd = fd;
  self->tag = g_source_add_unix_fd (source, fd,
                                    G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL);

  return g_steal_pointer (&source);
}

/**
 * eins_boottimeout_add_useconds:
 * @interval_us: the timeout interval, in microseconds
 * @function: function to call
 * @data: data to pass to @function
 *
 * Like g_timeout_add(), but uses `CLOCK_BOOTTIME` to account for time when the
 * system is suspended.
 *
 * If @interval_us is set to zero, the #GSource will be ready the next time it's
 * checked.
 *
 * Returns: the ID (greater than 0) of the event source.
 */
guint
eins_boottimeout_add_useconds (guint64     interval_us,
                               GSourceFunc function,
                               gpointer    data)
{
  guint id;
  g_autoptr(GError) error = NULL;
  GSource *source = eins_boottime_source_new_useconds (interval_us, &error);

  if (!source)
    {
      g_error ("Failed to have EINS boottime source: %s", error->message);
      return 0;
    }

  g_source_set_callback (source, function, data, NULL);
  id = g_source_attach (source, NULL);
  g_source_unref (source);

  return id;
}
