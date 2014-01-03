/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#ifndef EMTR_UTIL_H
#define EMTR_UTIL_H

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

#include "emtr-types.h"

#include <gio/gio.h>

G_BEGIN_DECLS

EMTR_ALL_API_VERSIONS
GFile    *emtr_get_default_storage_dir      (void);

EMTR_ALL_API_VERSIONS
GVariant *emtr_create_session_time_payload  (gint64       elapsed_time);

EMTR_ALL_API_VERSIONS
GVariant *emtr_create_app_usage_payload     (const gchar *activity_name,
                                             gint64       elapsed_time);

EMTR_ALL_API_VERSIONS
GVariant *emtr_aggregate_app_usage_payloads (GVariant   **payloads);

EMTR_ALL_API_VERSIONS
GVariant *emtr_create_feedback_payload      (const gchar *message,
                                             gboolean     is_bug);

G_END_DECLS

#endif /* EMTR_UTIL_H */
