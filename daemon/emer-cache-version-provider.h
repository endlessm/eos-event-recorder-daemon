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

#ifndef EMER_CACHE_VERSION_PROVIDER_H
#define EMER_CACHE_VERSION_PROVIDER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_CACHE_VERSION_PROVIDER emer_cache_version_provider_get_type()

#define EMER_CACHE_VERSION_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMER_TYPE_CACHE_VERSION_PROVIDER, EmerCacheVersionProvider))

#define EMER_CACHE_VERSION_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMER_TYPE_CACHE_VERSION_PROVIDER, EmerCacheVersionProviderClass))

#define EMER_IS_CACHE_VERSION_PROVIDER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMER_TYPE_CACHE_VERSION_PROVIDER))

#define EMER_IS_CACHE_VERSION_PROVIDER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMER_TYPE_CACHE_VERSION_PROVIDER))

#define EMER_CACHE_VERSION_PROVIDER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMER_TYPE_CACHE_VERSION_PROVIDER, EmerCacheVersionProviderClass))

/*
 * EmerCacheVersionProvider:
 *
 * This instance structure contains no public members.
 */
typedef struct _EmerCacheVersionProvider EmerCacheVersionProvider;

/*
 * EmerCacheVersionProviderClass:
 *
 * This class structure contains no public members.
 */
typedef struct _EmerCacheVersionProviderClass EmerCacheVersionProviderClass;


struct _EmerCacheVersionProvider
{
  /*< private >*/
  GObject parent;
};

struct _EmerCacheVersionProviderClass
{
  /*< private >*/
  GObjectClass parent_class;
};

GType                     emer_cache_version_provider_get_type    (void) G_GNUC_CONST;

EmerCacheVersionProvider *emer_cache_version_provider_new         (const gchar              *path);

gboolean                  emer_cache_version_provider_get_version (EmerCacheVersionProvider *self,
                                                                   gint                     *version);

gboolean                  emer_cache_version_provider_set_version (EmerCacheVersionProvider *self,
                                                                   gint                      new_version,
                                                                   GError                  **error);

G_END_DECLS

#endif /* EMER_CACHE_VERSION_PROVIDER_H */
