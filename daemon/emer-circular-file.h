/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2015-2017 Endless Mobile, Inc. */

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

#ifndef EMER_CIRCULAR_FILE_H
#define EMER_CIRCULAR_FILE_H

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define EMER_TYPE_CIRCULAR_FILE emer_circular_file_get_type()

#define EMER_CIRCULAR_FILE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  EMER_TYPE_CIRCULAR_FILE, EmerCircularFile))

#define EMER_CIRCULAR_FILE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), \
  EMER_TYPE_CIRCULAR_FILE, EmerCircularFileClass))

#define EMER_IS_CIRCULAR_FILE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
  EMER_TYPE_CIRCULAR_FILE))

#define EMER_IS_CIRCULAR_FILE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), \
  EMER_TYPE_CIRCULAR_FILE))

#define EMER_CIRCULAR_FILE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), \
  EMER_TYPE_CIRCULAR_FILE, EmerCircularFileClass))

#define METADATA_EXTENSION ".metadata"

typedef struct _EmerCircularFile EmerCircularFile;
typedef struct _EmerCircularFileClass EmerCircularFileClass;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EmerCircularFile, g_object_unref)

struct _EmerCircularFile
{
  GObject parent;
};

struct _EmerCircularFileClass
{
  GObjectClass parent_class;
};

GType             emer_circular_file_get_type (void) G_GNUC_CONST;

EmerCircularFile *emer_circular_file_new      (const gchar      *path,
                                               guint64           max_size,
                                               gboolean          reinitialize,
                                               GError          **error);

gboolean          emer_circular_file_append   (EmerCircularFile *self,
                                               gconstpointer     elem,
                                               guint64           elem_size);

gboolean          emer_circular_file_save     (EmerCircularFile *self,
                                               GError          **error);

gboolean          emer_circular_file_read     (EmerCircularFile *self,
                                               GBytes         ***elems,
                                               gsize             num_bytes,
                                               gsize            *num_elems,
                                               guint64          *token,
                                               gboolean         *has_invalid,
                                               GError          **error);

gboolean          emer_circular_file_has_more (EmerCircularFile *self,
                                               guint64           token);

gboolean          emer_circular_file_remove   (EmerCircularFile *self,
                                               guint64           token,
                                               GError          **error);

gboolean          emer_circular_file_purge    (EmerCircularFile *self,
                                               GError          **error);

G_END_DECLS

#endif /* EMER_CIRCULAR_FILE_H */
