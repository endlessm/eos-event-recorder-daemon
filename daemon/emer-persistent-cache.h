/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014, 2015 Endless Mobile, Inc. */

/*
 * This file is part of eos-event-recorder-daemon.
 *
 * eos-event-recorder-daemon is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 *
 * eos-event-recorder-daemon is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with eos-event-recorder-daemon.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef EMER_PERSISTENT_CACHE_H
#define EMER_PERSISTENT_CACHE_H

#include <glib-object.h>
#include <gio/gio.h>

#include "daemon/emer-boot-id-provider.h"
#include "daemon/emer-cache-size-provider.h"
#include "daemon/emer-cache-version-provider.h"
#include "shared/metrics-util.h"

G_BEGIN_DECLS

#define EMER_TYPE_PERSISTENT_CACHE emer_persistent_cache_get_type()

#define EMER_PERSISTENT_CACHE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                                                EMER_TYPE_PERSISTENT_CACHE, \
                                                                EmerPersistentCache))

#define EMER_PERSISTENT_CACHE_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), \
                                                                     EMER_TYPE_PERSISTENT_CACHE, \
                                                                     EmerPersistentCacheClass))

#define EMER_IS_PERSISTENT_CACHE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
                                                                   EMER_TYPE_PERSISTENT_CACHE))

#define EMER_IS_PERSISTENT_CACHE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), \
                                                                        EMER_TYPE_PERSISTENT_CACHE))

#define EMER_PERSISTENT_CACHE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), \
                                                                         EMER_TYPE_PERSISTENT_CACHE, \
                                                                         EmerPersistentCacheClass))

/*
 * The name of the metadata file containing the relative time, absolute time,
 * and boot id metadata.
 */
// FIXME: Rename to boot_offset_metadata since a metafile is something
// completely unrelated.
#define BOOT_OFFSET_METADATA_FILE "boot_offset_metafile"

/*
 * The prefix for all metrics cache files.
 */
#define CACHE_PREFIX "cache_"

/*
 * The suffixes for each metric type's file.
 */
#define INDIVIDUAL_SUFFIX "individual.metrics"
#define AGGREGATE_SUFFIX  "aggregate.metrics"
#define SEQUENCE_SUFFIX   "sequence.metrics"

/*
 * GVariant Types for deserializing metrics.
 */
#define INDIVIDUAL_TYPE "(uayxmv)"
#define AGGREGATE_TYPE  "(uayxxmv)"
#define SEQUENCE_TYPE   "(uaya(xmv))"

/*
 * Constants for GKeyFile metadata.
 */
#define CACHE_TIMING_GROUP_NAME   "time"
#define CACHE_ABSOLUTE_TIME_KEY   "absolute_time"
#define CACHE_RELATIVE_TIME_KEY   "relative_time"
#define CACHE_LAST_BOOT_ID_KEY    "boot_id"
#define CACHE_BOOT_OFFSET_KEY     "boot_offset"
#define CACHE_WAS_RESET_KEY       "was_reset"

typedef struct _EmerPersistentCache EmerPersistentCache;
typedef struct _EmerPersistentCacheClass EmerPersistentCacheClass;

/*
 * CAPAICTY_LOW = No urgent need to write to network.
 * CAPACITY_HIGH = Should write to network when possible. Occupancy is beyond a threshold.
 * CAPACITY_MAX = Should write to network when possible. Occupancy is at or
 *                near 100% and metrics are being ignored!
 */
typedef enum
{
  CAPACITY_LOW,
  CAPACITY_HIGH,
  CAPACITY_MAX
} capacity_t;

struct _EmerPersistentCache
{
  GObject parent;
};

struct _EmerPersistentCacheClass
{
  GObjectClass parent_class;
};

GType               emer_persistent_cache_get_type             (void) G_GNUC_CONST;

gboolean            emer_persistent_cache_get_boot_time_offset (EmerPersistentCache      *self,
                                                                gint64                   *offset,
                                                                GError                  **error,
                                                                gboolean                  always_update_timestamps);

EmerPersistentCache *emer_persistent_cache_new                 (GCancellable             *cancellable,
                                                                GError                  **error);

gboolean             emer_persistent_cache_drain_metrics       (EmerPersistentCache      *self,
                                                                GVariant               ***list_of_individual_metrics,
                                                                GVariant               ***list_of_aggregate_metrics,
                                                                GVariant               ***list_of_sequence_metrics,
                                                                gint                      max_num_bytes);

gboolean             emer_persistent_cache_store_metrics       (EmerPersistentCache      *self,
                                                                SingularEvent            *singular_buffer,
                                                                AggregateEvent           *aggregate_buffer,
                                                                SequenceEvent            *sequence_buffer,
                                                                gint                      num_singulars_buffered,
                                                                gint                      num_aggregates_buffered,
                                                                gint                      num_sequences_buffered,
                                                                gint                     *num_singulars_stored,
                                                                gint                     *num_aggregates_stored,
                                                                gint                     *num_sequences_stored,
                                                                capacity_t               *capacity);

EmerPersistentCache *emer_persistent_cache_new_full            (GCancellable             *cancellable,
                                                                GError                  **error,
                                                                const gchar              *custom_directory,
                                                                EmerCacheSizeProvider    *cache_size_provider,
                                                                EmerBootIdProvider       *boot_id_provider,
                                                                EmerCacheVersionProvider *version_provider,
                                                                guint                     boot_offset_update_interval);

G_END_DECLS

#endif /* EMER_PERSISTENT_CACHE_H */
