/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 Endless Mobile, Inc. */

#ifndef EMER_PERSISTENT_CACHE_H
#define EMER_PERSISTENT_CACHE_H

#include <glib-object.h>
#include <gio/gio.h>

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
 * The location of the meta-file containing the local cache meta-data.
 */
#define LOCAL_CACHE_VERSION_METAFILE "local_metafile"

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

GType                emer_persistent_cache_get_type                          (void) G_GNUC_CONST;

EmerPersistentCache *emer_persistent_cache_get_default                       (GCancellable         *cancellable,
                                                                              GError              **error);

gboolean             emer_persistent_cache_drain_metrics                     (EmerPersistentCache  *self,
                                                                              GVariant           ***list_of_individual_metrics,
                                                                              GVariant           ***list_of_aggregate_metrics,
                                                                              GVariant           ***list_of_sequence_metrics,
                                                                              gint                  max_num_bytes);

gboolean             emer_persistent_cache_store_metrics                     (EmerPersistentCache  *self,
                                                                              GVariant            **list_of_individual_metrics,
                                                                              GVariant            **list_of_aggregate_metrics,
                                                                              GVariant            **list_of_sequence_metrics,
                                                                              gint                 *num_individual_metrics_stored,
                                                                              gint                 *num_aggregate_metrics_stored,
                                                                              gint                 *num_sequence_metrics_stored,
                                                                              capacity_t           *capacity);
/*
 * Function should only be used in testing code, NOT in production code.
 */
EmerPersistentCache *emer_persistent_cache_new                               (GCancellable         *cancellable,
                                                                              GError              **error,
                                                                              gchar                *custom_directory,
                                                                              gint                  custom_cache_size);
/*
 * Function should only be used in testing code, NOT in production code.
 */
gboolean             emer_persistent_cache_set_different_version_for_testing (void);

G_END_DECLS

#endif /* EMER_PERSISTENT_CACHE_H */
