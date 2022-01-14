/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2015 Endless Mobile, Inc. */

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

#include "config.h"
#include "mock-circular-file.h"

#include <string.h>

#include <glib.h>

static GError *mock_circular_file_construct_error = NULL;
static gboolean mock_circular_file_reinitialize = FALSE;

typedef struct _EmerCircularFilePrivate
{
  guint8 *buffer;
  gsize max_size;
  gsize saved_size;
  gsize unsaved_size;
} EmerCircularFilePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerCircularFile, emer_circular_file,
                            G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_MAX_SIZE,
  NPROPS
};

static GParamSpec *emer_circular_file_props[NPROPS] = { NULL, };

static void
set_max_size (EmerCircularFile *self,
              guint64           max_size)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  priv->buffer = g_new (guint8, max_size);
  priv->max_size = max_size;
}

static void
emer_circular_file_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  EmerCircularFile *self = EMER_CIRCULAR_FILE (object);

  switch (property_id)
    {
    case PROP_MAX_SIZE:
      set_max_size (self, g_value_get_uint64 (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_circular_file_init (EmerCircularFile *self)
{
  /* Nothing to do */
}

static void
emer_circular_file_finalize (GObject *object)
{
  EmerCircularFile *self = EMER_CIRCULAR_FILE (object);
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  g_free (priv->buffer);

  G_OBJECT_CLASS (emer_circular_file_parent_class)->finalize (object);
}

static void
emer_circular_file_class_init (EmerCircularFileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = emer_circular_file_set_property;
  object_class->finalize = emer_circular_file_finalize;

  emer_circular_file_props[PROP_MAX_SIZE] =
    g_param_spec_uint64 ("max-size", "Max size",
                         "The maximum number of bytes that may be stored.",
                         0, G_MAXUINT, 0,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_circular_file_props);
}

EmerCircularFile *
emer_circular_file_new (const gchar *path,
                        guint64      max_size,
                        gboolean     reinitialize,
                        GError     **error)
{
  mock_circular_file_reinitialize = reinitialize;

  if (mock_circular_file_construct_error != NULL)
    {
      g_propagate_error (error,
                         g_steal_pointer (&mock_circular_file_construct_error));
      return NULL;
    }

  return g_object_new (EMER_TYPE_CIRCULAR_FILE,
                       "max-size", max_size,
                       NULL);
}

gboolean
emer_circular_file_append (EmerCircularFile *self,
                           gconstpointer     elem,
                           guint64           elem_size)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  gsize new_unsaved_size = priv->unsaved_size + sizeof (elem_size) + elem_size;
  if (priv->saved_size + new_unsaved_size > priv->max_size)
    return FALSE;

  guint8 *tail = priv->buffer + priv->saved_size + priv->unsaved_size;
  memcpy (tail, &elem_size, sizeof (elem_size));
  memcpy (tail + sizeof (elem_size), elem, elem_size);
  priv->unsaved_size = new_unsaved_size;
  return TRUE;
}

gboolean
emer_circular_file_save (EmerCircularFile *self,
                         GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  priv->saved_size += priv->unsaved_size;
  priv->unsaved_size = 0;
  return TRUE;
}

gboolean
emer_circular_file_read (EmerCircularFile *self,
                         GBytes         ***elems,
                         gsize             num_bytes,
                         gsize            *num_elems,
                         guint64          *token,
                         gboolean         *has_invalid,
                         GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  GPtrArray *elem_array = g_ptr_array_new ();
  gsize curr_elem_bytes = 0;
  guint64 curr_buffer_bytes = 0;
  while (curr_buffer_bytes < priv->saved_size)
    {
      guint64 elem_size;
      gsize new_elem_bytes;

      /* Since we end up adding @elem_size, which is read from the buffer, to
       * @curr_buffer_bytes below, we can’t guarantee alignment here. Use
       * memcpy() to avoid unaligned accesses (and hence SIGBUS) on ARM. */
      memcpy (&elem_size, priv->buffer + curr_buffer_bytes, sizeof (elem_size));
      new_elem_bytes = curr_elem_bytes + elem_size;

      if (new_elem_bytes > num_bytes)
        break;

      curr_elem_bytes = new_elem_bytes;

      /* sizeof (elem_size) gives the number of bytes used to record the
       * element's length.
       */
      curr_buffer_bytes += sizeof (elem_size);

      GBytes *elem = g_bytes_new (priv->buffer + curr_buffer_bytes, elem_size);
      curr_buffer_bytes += elem_size;
      g_ptr_array_add (elem_array, elem);
    }

  *num_elems = elem_array->len;
  *elems = (GBytes **) g_ptr_array_free (elem_array, FALSE);
  *token = curr_buffer_bytes;
  *has_invalid = FALSE;
  return TRUE;
}

gboolean
emer_circular_file_has_more (EmerCircularFile *self,
                             guint64           token)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  return token < priv->saved_size;
}

gboolean
emer_circular_file_remove (EmerCircularFile *self,
                           guint64           token,
                           GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  priv->saved_size -= token;
  gsize bytes_remaining = priv->saved_size + priv->unsaved_size;
  memmove (priv->buffer, priv->buffer + token, bytes_remaining);
  return TRUE;
}

gboolean
emer_circular_file_purge (EmerCircularFile *self,
                          GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  priv->saved_size = 0;
  return TRUE;
}

/* Sets an error to raise from the next call to emer_circular_file_new().
 */
void
mock_circular_file_set_construct_error (const GError *error)
{
  g_clear_error (&mock_circular_file_construct_error);

  if (error != NULL)
    mock_circular_file_construct_error = g_error_copy (error);
}

gboolean
mock_circular_file_got_reinitialize (void)
{
  return mock_circular_file_reinitialize;
}
