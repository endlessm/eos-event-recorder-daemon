/* Copyright 2014 Endless Mobile, Inc. */

/* Emtr_Persistent_cache_private.h */

#ifndef __EMTR_PERSISTENT_CACHE_H__
#define __EMTR_PERSISTENT_CACHE_H__

#if !(defined(_EMTR_INSIDE_EOSMETRICS_H) || defined(COMPILING_EOS_METRICS))
#error "Please do not include this header file directly."
#endif

#include "emtr-types.h"
#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define EMTR_TYPE_PERSISTENT_CACHE emtr_persistent_cache_get_type()

#define EMTR_PERSISTENT_CACHE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), EMTR_TYPE_PERSISTENT_CACHE, Emtr_Persistent_Cache))

#define EMTR_PERSISTENT_CACHE_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), EMTR_TYPE_PERSISTENT_CACHE, Emtr_Persistent_CacheClass))

#define EMTR_IS_PERSISTENT_CACHE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), EMTR_TYPE_PERSISTENT_CACHE))

#define EMTR_IS_PERSISTENT_CACHE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), EMTR_TYPE_PERSISTENT_CACHE))

#define EMTR_PERSISTENT_CACHE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), EMTR_TYPE_PERSISTENT_CACHE, Emtr_Persistent_CacheClass))

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
#define INDIVIDUAL_TYPE "(ayxmv)"
#define AGGREGATE_TYPE  "(ayxxmv)"
#define SEQUENCE_TYPE   "(aya(xmv))"

typedef struct _Emtr_Persistent_Cache Emtr_Persistent_Cache;
typedef struct _Emtr_Persistent_CacheClass Emtr_Persistent_CacheClass;
typedef struct _Emtr_Persistent_CachePrivate Emtr_Persistent_CachePrivate;

/*
 * CAPAICTY_LOW = No urgent need to write to network.
 * CAPACITY_HIGH = Should write to network when possible. Occupancy is beyond a threshold.
 * CAPACITY_MAX = Should write to network when possible. Occupancy is at or 
 *                near 100% and metrics are being ignored!
 */
typedef enum {CAPACITY_LOW, CAPACITY_HIGH, CAPACITY_MAX} capacity_t;

struct _Emtr_Persistent_Cache
{
  GObject parent;
};

struct _Emtr_Persistent_CacheClass
{
  GObjectClass parent_class;
};

GType emtr_persistent_cache_get_type (void) G_GNUC_CONST;

Emtr_Persistent_Cache *emtr_persistent_cache_get_default                       (GCancellable           *cancellable,
                                                                                GError                **error);

gboolean               emtr_persistent_cache_drain_metrics                     (Emtr_Persistent_Cache  *self,
                                                                                GVariant             ***list_of_individual_metrics,
                                                                                GVariant             ***list_of_aggregate_metrics,
                                                                                GVariant             ***list_of_sequence_metrics);

gboolean               emtr_persistent_cache_store_metrics                     (Emtr_Persistent_Cache  *self,
                                                                                GVariant              **list_of_individual_metrics,
                                                                                GVariant              **list_of_aggregate_metrics,
                                                                                GVariant              **list_of_sequence_metrics,
                                                                                capacity_t             *capacity);
/*
 * Function should only be used in testing code, NOT in production code.
 */
Emtr_Persistent_Cache* emtr_persistent_cache_new                              (GCancellable           *cancellable,
                                                                               GError                **error,
                                                                               gchar                  *custom_directory,
                                                                               guint                   custom_cache_size);
/*
 * Function should only be used in testing code, NOT in production code.
 */
gboolean               emtr_persistent_cache_set_different_version_for_testing (Emtr_Persistent_Cache *self);

G_END_DECLS

#endif /* __EMTR_PERSISTENT_CACHE_H__ */
