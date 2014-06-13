/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#ifndef METRICS_UTIL_H
#define METRICS_UTIL_H

/* For clockid_t */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 199309L
#error "This code requires _POSIX_C_SOURCE to be 199309L or later."
#endif

#include <sys/types.h>

#include <glib.h>
#include <gio/gio.h>
#include <uuid/uuid.h>

/* The number of nanoseconds in one second. */
#define NANOSECONDS_PER_SECOND 1000000000L

/* The number of elements in a uuid_t. uuid_t is assumed to be a fixed-length
array of guchar. */
#define UUID_LENGTH (sizeof (uuid_t) / sizeof (guchar))

G_BEGIN_DECLS

void      get_builder_from_iter          (GVariantIter       *iter,
                                          GVariantBuilder    *builder,
                                          const GVariantType *type);

gboolean  get_current_time               (clockid_t           clock_id,
                                          gint64             *current_time);

void      get_uuid_builder               (uuid_t              uuid,
                                          GVariantBuilder    *uuid_builder);

G_END_DECLS

#endif /* METRICS_UTIL_H */
