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

#define DEFAULT_CONFIG_FILE_PATH SYSCONFDIR "/eos-metrics-permissions.conf"

G_BEGIN_DECLS

typedef struct EventValue
{
  // Time elapsed in nanoseconds from an unspecified starting point.
  gint64 relative_timestamp;

  GVariant *auxiliary_payload;
} EventValue;

typedef struct SingularEvent
{
  guint32 user_id;
  uuid_t event_id;
  EventValue event_value;
} SingularEvent;

typedef struct AggregateEvent
{
  SingularEvent event;
  gint64 num_events;
} AggregateEvent;

typedef struct SequenceEvent
{
  guint32 user_id;
  uuid_t event_id;

  /*
   * The first element is the start event, the last element is the stop event,
   * and any elements in between are progress events. The elements are ordered
   * chronologically.
   */
  EventValue *event_values;

  gsize num_event_values;
} SequenceEvent;

void       trash_singular_event  (SingularEvent   *singular);

void       trash_aggregate_event (AggregateEvent  *aggregate);

void       trash_sequence_event  (SequenceEvent   *sequence);

void       free_singular_buffer  (SingularEvent   *singular_buffer,
                                  gint             num_singulars_buffered);

void       free_aggregate_buffer (AggregateEvent  *aggregate_buffer,
                                  gint             num_aggregates_buffered);

void       free_sequence_buffer  (SequenceEvent   *sequence_buffer,
                                  gint             num_sequences_buffered);

GVariant * singular_to_variant   (SingularEvent   *singular);

GVariant * aggregate_to_variant  (AggregateEvent  *aggregate);

GVariant * sequence_to_variant   (SequenceEvent   *sequence);

void       get_uuid_builder      (uuid_t           uuid,
                                  GVariantBuilder *uuid_builder);

gboolean   get_current_time      (clockid_t        clock_id,
                                  gint64          *current_time);

G_END_DECLS

#endif /* METRICS_UTIL_H */
