/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef EMTR_EVENT_RECORDER_H
#define EMTR_EVENT_RECORDER_H

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

#include "emtr-types.h"
#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define EMTR_TYPE_EVENT_RECORDER emtr_event_recorder_get_type()

#define EMTR_EVENT_RECORDER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMTR_TYPE_EVENT_RECORDER, EmtrEventRecorder))

#define EMTR_EVENT_RECORDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMTR_TYPE_EVENT_RECORDER, EmtrEventRecorderClass))

#define EMTR_IS_EVENT_RECORDER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMTR_TYPE_EVENT_RECORDER))

#define EMTR_IS_EVENT_RECORDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMTR_TYPE_EVENT_RECORDER))

#define EMTR_EVENT_RECORDER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMTR_TYPE_EVENT_RECORDER, EmtrEventRecorderClass))

/**
 * EmtrEventRecorder:
 *
 * This instance structure contains no public members.
 */
typedef struct _EmtrEventRecorder EmtrEventRecorder;

/**
 * EmtrEventRecorderClass:
 *
 * This class structure contains no public members.
 */
typedef struct _EmtrEventRecorderClass EmtrEventRecorderClass;

struct _EmtrEventRecorder
{
  /*< private >*/
  GObject parent;
};

struct _EmtrEventRecorderClass
{
  /*< private >*/
  GObjectClass parent_class;
};

EMTR_ALL_API_VERSIONS
GType              emtr_event_recorder_get_type        (void) G_GNUC_CONST;

EMTR_ALL_API_VERSIONS
EmtrEventRecorder *emtr_event_recorder_new             (void);

EMTR_ALL_API_VERSIONS
void               emtr_event_recorder_record_event    (EmtrEventRecorder *self,
                                                        const gchar       *event_id,
                                                        GVariant          *auxiliary_payload);

EMTR_ALL_API_VERSIONS
void               emtr_event_recorder_record_events   (EmtrEventRecorder *self,
                                                        const gchar       *event_id,
                                                        gint64             num_events,
                                                        GVariant          *auxiliary_payload);

EMTR_ALL_API_VERSIONS
void               emtr_event_recorder_record_start    (EmtrEventRecorder *self,
                                                        const gchar       *event_id,
                                                        GVariant          *key,
                                                        GVariant          *auxiliary_payload);

EMTR_ALL_API_VERSIONS
void               emtr_event_recorder_record_progress (EmtrEventRecorder *self,
                                                        const gchar       *event_id,
                                                        GVariant          *key,
                                                        GVariant          *auxiliary_payload);

EMTR_ALL_API_VERSIONS
void               emtr_event_recorder_record_stop     (EmtrEventRecorder *self,
                                                        const gchar       *event_id,
                                                        GVariant          *key,
                                                        GVariant          *auxiliary_payload);

G_END_DECLS

#endif /* EMTR_EVENT_RECORDER_H */
