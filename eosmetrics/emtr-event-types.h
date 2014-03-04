/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef EMTR_EVENT_TYPES_H
#define EMTR_EVENT_TYPES_H

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

G_BEGIN_DECLS

/**
 * SECTION:emtr-event-types
 * @title: Event Types
 * @short_description: shared constant definitions for event types
 *
 * Event types are RFC 4122 UUIDs. This file provides a mapping from
 * 36-character string representations of UUIDs to human-readable constants. New
 * event types should be registered here. UUIDs should never be recycled since
 * this will create confusion when analyzing the metrics database. The list is
 * sorted alphabetically by name.
 *
 * To generate a new UUID on Endless OS, Debian, or Ubuntu:
 * |[
 * sudo apt-get install uuid-runtime
 * uuidgen -r
 * ]|
 */

/**
 * EMTR_EVENT_USER_LOGGED_IN:
 *
 * Started when a user logs in and stopped when that user logs out.
 */
#define EMTR_EVENT_USER_LOGGED_IN "ab839fd2-a927-456c-8c18-f1136722666b"

G_END_DECLS

#endif /* EMTR_EVENT_TYPES_H */
