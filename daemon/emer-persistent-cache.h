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

#include "emer-boot-id-provider.h"
#include "emer-cache-size-provider.h"
#include "emer-cache-version-provider.h"
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

struct _EmerPersistentCache
{
  GObject parent;
};

struct _EmerPersistentCacheClass
{
  GObjectClass parent_class;
};

GType                emer_persistent_cache_get_type             (void) G_GNUC_CONST;

EmerPersistentCache *emer_persistent_cache_new                  (const gchar              *directory,
                                                                 GError                  **error);

gsize                emer_persistent_cache_cost                 (GVariant                 *self);

gboolean             emer_persistent_cache_get_boot_time_offset (EmerPersistentCache      *self,
                                                                 gint64                   *offset,
                                                                 gboolean                  always_update_timestamps,
                                                                 GError                  **error);

gboolean             emer_persistent_cache_store                (EmerPersistentCache      *self,
                                                                 GVariant                **variants,
                                                                 gsize                     num_variants,
                                                                 gsize                    *num_variants_stored,
                                                                 GError                  **error);

gboolean             emer_persistent_cache_read                 (EmerPersistentCache      *self,
                                                                 GVariant               ***variants,
                                                                 gsize                     cost,
                                                                 gsize                    *num_variants,
                                                                 guint64                  *token,
                                                                 GError                  **error);

gboolean             emer_persistent_cache_has_more             (EmerPersistentCache      *self,
                                                                 guint64                   token);

gboolean             emer_persistent_cache_remove               (EmerPersistentCache      *self,
                                                                 guint64                   token,
                                                                 GError                  **error);

EmerPersistentCache *emer_persistent_cache_new_full             (const gchar              *directory,
                                                                 EmerCacheSizeProvider    *cache_size_provider,
                                                                 EmerBootIdProvider       *boot_id_provider,
                                                                 EmerCacheVersionProvider *version_provider,
                                                                 guint                     boot_offset_update_interval,
                                                                 GError                  **error);

G_END_DECLS

#endif /* EMER_PERSISTENT_CACHE_H */
