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

static void
trash_event_value (EventValue *event_value)
{
  GVariant *auxiliary_payload = event_value->auxiliary_payload;
  if (auxiliary_payload != NULL)
    g_variant_unref (auxiliary_payload);
}

void
trash_singular_event (SingularEvent *singular)
{
  EventValue *event_value = &singular->event_value;
  trash_event_value (event_value);
}

void
trash_aggregate_event (AggregateEvent *aggregate)
{
  SingularEvent *event = &aggregate->event;
  trash_singular_event (event);
}

void
trash_sequence_event (SequenceEvent *sequence)
{
  for (gint i = 0; i < sequence->num_event_values; ++i)
    trash_event_value (sequence->event_values + i);

  g_free (sequence->event_values);
}

void
free_singular_buffer (SingularEvent *singular_buffer,
                      gint           num_singulars_buffered)
{
  for (gint i = 0; i < num_singulars_buffered; i++)
    trash_singular_event (singular_buffer + i);

  g_free (singular_buffer);
}

void
free_aggregate_buffer (AggregateEvent *aggregate_buffer,
                       gint            num_aggregates_buffered)
{
  for (gint i = 0; i < num_aggregates_buffered; i++)
    trash_aggregate_event (aggregate_buffer + i);

  g_free (aggregate_buffer);
}

void
free_sequence_buffer (SequenceEvent *sequence_buffer,
                      gint           num_sequences_buffered)
{
  for (gint i = 0; i < num_sequences_buffered; i++)
    trash_sequence_event (sequence_buffer + i);

  g_free (sequence_buffer);
}

GVariant *
singular_to_variant (SingularEvent *singular)
{
  GVariantBuilder event_id_builder;
  get_uuid_builder (singular->event_id, &event_id_builder);
  EventValue event_value = singular->event_value;
  return g_variant_new ("(uayxmv)", singular->user_id, &event_id_builder,
                        event_value.relative_timestamp,
                        event_value.auxiliary_payload);
}

GVariant *
aggregate_to_variant (AggregateEvent *aggregate)
{
  SingularEvent event = aggregate->event;
  GVariantBuilder event_id_builder;
  get_uuid_builder (event.event_id, &event_id_builder);
  EventValue event_value = event.event_value;
  return g_variant_new ("(uayxxmv)", event.user_id, &event_id_builder,
                        event_value.relative_timestamp, aggregate->num_events,
                        event_value.auxiliary_payload);
}

GVariant *
sequence_to_variant (SequenceEvent *sequence)
{
  GVariantBuilder event_values_builder;
  g_variant_builder_init (&event_values_builder, G_VARIANT_TYPE ("a(xmv)"));
  for (gint i = 0; i < sequence->num_event_values; i++)
    {
      EventValue event_value = sequence->event_values[i];
      g_variant_builder_add (&event_values_builder, "(xmv)",
                             event_value.relative_timestamp,
                             event_value.auxiliary_payload);
    }

  GVariantBuilder event_id_builder;
  get_uuid_builder (sequence->event_id, &event_id_builder);

  return g_variant_new ("(uaya(xmv))", sequence->user_id, &event_id_builder,
                        &event_values_builder);

}

void
get_uuid_builder (uuid_t           uuid,
                  GVariantBuilder *uuid_builder)
{
  g_variant_builder_init (uuid_builder, G_VARIANT_TYPE ("ay"));
  for (size_t i = 0; i < UUID_LENGTH; ++i)
    g_variant_builder_add (uuid_builder, "y", uuid[i]);
}

/*
 * Populates builder with the elements from iter. Assumes all elements are of
 * the given type.
 */
void
get_builder_from_iter (GVariantIter       *iter,
                       GVariantBuilder    *builder,
                       const GVariantType *type)
{
  g_variant_builder_init (builder, type);
  while (TRUE)
    {
      GVariant *curr_elem = g_variant_iter_next_value (iter);
      if (curr_elem == NULL)
        break;
      g_variant_builder_add_value (builder, curr_elem);
      g_variant_unref (curr_elem);
    }
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

  gint64 detected_time = (NANOSECONDS_PER_SECOND * ((gint64) ts.tv_sec))
                          + ((gint64) ts.tv_nsec);
  if (detected_time < (G_MININT64 / 2) || detected_time > (G_MAXINT64 / 2))
    {
      g_critical ("Clock returned a time that may result in arithmetic that "
                  "causes 64-bit overflow. This machine may have been running "
                  "for over 100 years! (Has a bird pooped in your mouth?)");
      return FALSE;
    }
  *current_time = detected_time;
  return TRUE;
}
