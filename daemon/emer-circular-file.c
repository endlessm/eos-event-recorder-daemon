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

#include "emer-circular-file.h"

#include <gio/gio.h>
#include <glib.h>

#include "shared/metrics-util.h"

#define METADATA_GROUP_NAME "metadata"
#define MAX_SIZE_KEY "max_size"
#define SIZE_KEY "size"
#define HEAD_KEY "head"

typedef struct _EmerCircularFilePrivate
{
  GFile *data_file;
  GKeyFile *metadata_key_file;
  gchar *metadata_filepath;

  GByteArray *write_buffer;

  guint64 max_size;
  guint64 size;
  goffset head;

  gboolean reinitialize;
} EmerCircularFilePrivate;

static void emer_circular_file_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (EmerCircularFile, emer_circular_file, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (EmerCircularFile)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, emer_circular_file_initable_iface_init))

enum
{
  PROP_0,
  PROP_PATH,
  PROP_MAX_SIZE,
  PROP_REINITIALIZE,
  NPROPS
};

static GParamSpec *emer_circular_file_props[NPROPS] = { NULL, };

static gboolean
save_metadata_file (EmerCircularFile *self,
                    GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  return g_key_file_save_to_file (priv->metadata_key_file,
                                  priv->metadata_filepath, error);
}

static gboolean
add_to_size (EmerCircularFile *self,
             guint64           delta,
             GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  guint64 new_size = priv->size + delta;
  g_key_file_set_uint64 (priv->metadata_key_file, METADATA_GROUP_NAME, SIZE_KEY,
                         new_size);
  if (!save_metadata_file (self, error))
    return FALSE;

  priv->size = new_size;
  return TRUE;
}

static gboolean
set_metadata (EmerCircularFile *self,
              guint64           size,
              goffset           head,
              GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  g_key_file_set_uint64 (priv->metadata_key_file, METADATA_GROUP_NAME, SIZE_KEY,
                         size);
  g_key_file_set_int64 (priv->metadata_key_file, METADATA_GROUP_NAME, HEAD_KEY,
                        head);

  if (!save_metadata_file (self, error))
    return FALSE;

  priv->size = size;
  priv->head = head;
  return TRUE;
}

static gboolean
read_disk_bytes (EmerCircularFile *self,
                 guint8           *buffer,
                 gsize             num_bytes,
                 gsize             max_size,
                 GError          **error)
{
  g_autoptr(GFileInputStream) file_input_stream = NULL;
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  if (num_bytes == 0)
    return TRUE;

  file_input_stream = g_file_read (priv->data_file, NULL /* GCancellable */, error);
  if (file_input_stream == NULL)
    return FALSE;

  GInputStream *input_stream = G_INPUT_STREAM (file_input_stream);
  gsize head_to_end = max_size - priv->head;
  gsize bytes_head = MIN(num_bytes, head_to_end);
  gsize bytes_start = num_bytes - bytes_head;
  if (bytes_start > 0)
    {
      gpointer copy_to = buffer + bytes_head;
      gboolean read_succeeded =
        g_input_stream_read_all (input_stream, copy_to, bytes_start,
                                 NULL /* bytes read */, NULL /* GCancellable */,
                                 error);
      if (!read_succeeded)
        return FALSE;
    }

  GSeekable *seekable = G_SEEKABLE (file_input_stream);
  gboolean seek_succeeded =
    g_seekable_seek (seekable, priv->head, G_SEEK_SET, NULL /* GCancellable */,
                     error);
  if (!seek_succeeded)
    return FALSE;

  gboolean read_succeeded =
    g_input_stream_read_all (input_stream, buffer, bytes_head,
                             NULL /* bytes read */, NULL /* GCancellable */,
                             error);

  return read_succeeded;
}

/* Returns the size of a length-encoded buffer excluding any truncated trailing
 * element. Assumes each element in the buffer is preceded by its length in
 * bytes encoded as a little-endian guint64. Also assumes that each length,
 * element pair is concatenated to the next.
 *
 * Since the encoded length of each buffer might contain an arbitrary value,
 * we need to be careful about doing unaligned accesses to avoid SIGBUS on ARM.
 * Use memcpy() to do this.
 */
static guint64
get_trimmed_size (const guint8 *buffer,
                  gsize         num_bytes)
{
  guint64 curr_pos = 0;
  while ((curr_pos + sizeof (curr_pos)) < num_bytes)
    {
      guint64 little_endian_elem_size, elem_size, next_pos;

      memcpy (&little_endian_elem_size, buffer + curr_pos, sizeof (little_endian_elem_size));
      elem_size = swap_bytes_64_if_big_endian (little_endian_elem_size);
      next_pos = curr_pos + sizeof (curr_pos) + elem_size;
      if (next_pos > num_bytes)
        break;
      curr_pos = next_pos;
    }

  return curr_pos;
}

/* Replace the data file with the contents of the given buffer, and update the
 * metadata file accordingly. In the event of an error, the circular file may
 * instead be left logically empty, but the data and metadata files will never
 * be left in an inconsistent state.
 */
static gboolean
overwrite (EmerCircularFile *self,
           gconstpointer     buffer,
           gsize             num_bytes,
           guint64           prev_max_size,
           GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  /* The size of the data file may not exceed the maximum size stored in the
   * metadata file.
   */
  if (priv->max_size > prev_max_size)
    g_key_file_set_uint64 (priv->metadata_key_file, METADATA_GROUP_NAME,
                           MAX_SIZE_KEY, priv->max_size);
  if (!set_metadata (self, 0, 0, error))
    return FALSE;

  gboolean write_succeeded =
    g_file_replace_contents (priv->data_file, buffer, num_bytes, NULL, FALSE,
                             G_FILE_CREATE_NONE, NULL, NULL, error);
  if (!write_succeeded)
    return FALSE;

  /* We only need to update the maximum size if we didn't already. */
  if (priv->max_size < prev_max_size)
    g_key_file_set_uint64 (priv->metadata_key_file, METADATA_GROUP_NAME,
                           MAX_SIZE_KEY, priv->max_size);

  return add_to_size (self, num_bytes, error);
}

/* Change the maximum size of the circular file from prev_max_size to
 * priv->max_size. If the new maximum is less than the amount of data currently
 * in the buffer, then any data that doesn't fit will be removed. Also
 * reorganizes the circular file so that its head is at the start of the file.
 */
static gboolean
resize (EmerCircularFile *self,
        guint64           prev_max_size,
        GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  if (prev_max_size == priv->max_size)
    return TRUE;

  gsize bytes_to_read = MIN (priv->size, priv->max_size);
  guint8 buffer[bytes_to_read];
  gboolean read_succeeded =
    read_disk_bytes (self, buffer, bytes_to_read, prev_max_size, error);
  if (!read_succeeded)
    return FALSE;

  guint64 new_size = get_trimmed_size (buffer, bytes_to_read);
  return overwrite (self, buffer, new_size, prev_max_size, error);
}

static gboolean
continue_reading_from_start (EmerCircularFile *self,
                             GInputStream     *input_stream,
                             guint8           *partially_filled_buffer,
                             gsize             total_bytes_to_read,
                             gsize             bytes_read_end,
                             GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  if (bytes_read_end == total_bytes_to_read)
    return TRUE;

  GSeekable *seekable = G_SEEKABLE (input_stream);
  goffset curr_position = g_seekable_tell (seekable);
  if (curr_position < 0 || (guint64) curr_position != priv->max_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Circular file has a physical size of %" G_GOFFSET_FORMAT
                   "bytes, but expected physical size to be %" G_GUINT64_FORMAT
                   " bytes.", curr_position, priv->max_size);
      return FALSE;
    }

  gboolean seek_succeeded =
    g_seekable_seek (seekable, 0, G_SEEK_SET, NULL /* GCancellable */, error);
  if (!seek_succeeded)
    return FALSE;

  guint8 *copy_to = partially_filled_buffer + bytes_read_end;
  gsize bytes_remaining = total_bytes_to_read - bytes_read_end;
  gsize bytes_read_start;
  gboolean read_succeeded =
    g_input_stream_read_all (input_stream, copy_to, bytes_remaining,
                             &bytes_read_start, NULL /* GCancellable */, error);
  if (!read_succeeded)
    return FALSE;

  if (bytes_remaining != bytes_read_start)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Circular file is shorter than expected. Reached end of "
                   "file at byte %" G_GSIZE_FORMAT ".", bytes_read_start);
      return FALSE;
    }

  return TRUE;
}

static gboolean
read_elem_size (EmerCircularFile *self,
                GInputStream     *input_stream,
                guint64          *elem_size,
                GError          **error)
{
  guint64 little_endian_elem_size;
  gsize bytes_read;
  gboolean read_succeeded =
    g_input_stream_read_all (input_stream,
                             &little_endian_elem_size,
                             sizeof (little_endian_elem_size),
                             &bytes_read,
                             NULL /* GCancellable */,
                             error);
  if (!read_succeeded)
    return FALSE;

  read_succeeded =
    continue_reading_from_start (self,
                                 input_stream,
                                 (guint8 *) &little_endian_elem_size,
                                 sizeof (little_endian_elem_size),
                                 bytes_read,
                                 error);
  if (!read_succeeded)
    return FALSE;

  *elem_size = swap_bytes_64_if_big_endian (little_endian_elem_size);
  return TRUE;
}

static gboolean
append_elem_to_array (EmerCircularFile *self,
                      GInputStream     *input_stream,
                      GPtrArray        *elem_array,
                      guint64           elem_size,
                      GError          **error)
{
  guint8 elem_data[elem_size];
  gsize bytes_read;
  gboolean read_succeeded =
    g_input_stream_read_all (input_stream, elem_data, elem_size, &bytes_read,
                             NULL /* GCancellable */, error);
  if (!read_succeeded)
    return FALSE;

  read_succeeded =
    continue_reading_from_start (self, input_stream, elem_data, elem_size,
                                 bytes_read, error);
  if (!read_succeeded)
    return FALSE;

  GBytes *elem = g_bytes_new (elem_data, elem_size);
  g_ptr_array_add (elem_array, elem);
  return TRUE;
}

static void
set_path (EmerCircularFile *self,
          const gchar      *data_filepath)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  priv->data_file = g_file_new_for_path (data_filepath);
  priv->metadata_filepath =
    g_strconcat (data_filepath, METADATA_EXTENSION, NULL);
}

static void
set_max_size (EmerCircularFile *self,
              guint64           max_size)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  priv->max_size = max_size;
}

static void
emer_circular_file_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  EmerCircularFile *self = EMER_CIRCULAR_FILE (object);
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  switch (property_id)
    {
    case PROP_PATH:
      set_path (self, g_value_get_string (value));
      break;

    case PROP_MAX_SIZE:
      set_max_size (self, g_value_get_uint64 (value));
      break;

    case PROP_REINITIALIZE:
      priv->reinitialize = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_circular_file_finalize (GObject *object)
{
  EmerCircularFile *self = EMER_CIRCULAR_FILE (object);
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  g_clear_object (&priv->data_file);
  g_clear_pointer (&priv->metadata_key_file, g_key_file_unref);
  g_clear_pointer (&priv->metadata_filepath, g_free);
  g_clear_pointer (&priv->write_buffer, g_byte_array_unref);

  G_OBJECT_CLASS (emer_circular_file_parent_class)->finalize (object);
}

static void
emer_circular_file_class_init (EmerCircularFileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = emer_circular_file_set_property;
  object_class->finalize = emer_circular_file_finalize;

  emer_circular_file_props[PROP_PATH] =
    g_param_spec_string ("path", "Path",
                         "Path at which circular file is stored",
                         NULL,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  emer_circular_file_props[PROP_MAX_SIZE] =
    g_param_spec_uint64 ("max-size", "Max size",
                         "The maximum permitted physical size of the "
                         "underlying data file. Does not include the overhead "
                         "of the metadata file.",
                         0, G_MAXUINT64, 0,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  emer_circular_file_props[PROP_REINITIALIZE] =
    g_param_spec_boolean ("reinitialize", "Reinitialize",
                          "Reinitialize the underlying data and metadata "
                          "files, if they already exist. This is intended as a "
                          "recovery mechanism if an existing file is corrupt "
                          "and can't be opened.",
                          FALSE,
                          G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                          G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_circular_file_props);
}

static void
emer_circular_file_init (EmerCircularFile *self)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  priv->write_buffer = g_byte_array_new ();
}

static gboolean
emer_circular_file_initable_init (GInitable    *initable,
                                  GCancellable *cancellable,
                                  GError      **error)
{
  EmerCircularFile *self = EMER_CIRCULAR_FILE (initable);
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  /* Create data file if it doesn't already exist. */
  GFileOutputStream *data_file_output_stream =
    g_file_append_to (priv->data_file, G_FILE_CREATE_NONE,
                      NULL /* GCancellable */, error);
  if (data_file_output_stream == NULL)
    return FALSE;

  g_object_unref (data_file_output_stream);

  priv->metadata_key_file = g_key_file_new ();
  g_autoptr(GError) local_error = NULL;

  if (!priv->reinitialize)
    {
      if (!g_key_file_load_from_file (priv->metadata_key_file,
                                      priv->metadata_filepath, G_KEY_FILE_NONE,
                                      &local_error))
        {
          /* If the metadata file just doesn't exist, this is fine: we just
           * need to initialize it.
           */
          if (!g_error_matches (local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }

          g_clear_error (&local_error);
          priv->reinitialize = TRUE;
        }
      else
        {
          /* If the metadata file exists but is empty, treat this as if it
           * didn't exist yet. This can occur if the system crashed after the
           * file was first initialized, but before any events were logged to
           * the file.
           */
          gsize n_groups;
          g_autofree GStrv groups =
            g_key_file_get_groups (priv->metadata_key_file, &n_groups);

          if (n_groups == 0)
            priv->reinitialize = TRUE;
        }
    }

  /* Either the :reinitialize construct-time property was set to TRUE, or one
   * of the cases above told us that we need to initialize the metadata file.
   * We don't need to modify the data file: we ensured it existed above, which
   * is enough.
   */
  if (priv->reinitialize)
    {
      g_key_file_set_uint64 (priv->metadata_key_file, METADATA_GROUP_NAME,
                             MAX_SIZE_KEY, priv->max_size);
      return set_metadata (self, 0, 0, error);
    }

  guint64 prev_max_size =
    g_key_file_get_uint64 (priv->metadata_key_file, METADATA_GROUP_NAME,
                           MAX_SIZE_KEY, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  priv->size =
    g_key_file_get_uint64 (priv->metadata_key_file, METADATA_GROUP_NAME,
                           SIZE_KEY, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (priv->size > prev_max_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Logical size "
                   "of circular file must be at most %" G_GUINT64_FORMAT ", "
                   "but was %" G_GUINT64_FORMAT ".", prev_max_size, priv->size);
      return FALSE;
    }

  priv->head =
    g_key_file_get_int64 (priv->metadata_key_file, METADATA_GROUP_NAME,
                          HEAD_KEY, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  if (priv->head < 0 || (guint64) priv->head >= prev_max_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Pointer to "
                   "head of circular file must lie in range [0, %"
                   G_GUINT64_FORMAT "), but was %" G_GOFFSET_FORMAT ".",
                   prev_max_size, priv->head);
      return FALSE;
    }

  return resize (self, prev_max_size, error);
}

static void
emer_circular_file_initable_iface_init (GInitableIface *iface)
{
  iface->init = emer_circular_file_initable_init;
}

/* Returns a new circular file or NULL on error. If a circular file does not
 * already exist at the given path, a new one is created. Limits the physical
 * size of the underlying data file to max_size bytes. If a circular file with a
 * different maximum size already exists at the given path, its maximum size is
 * changed to the given value, which may result in data loss.
 */
EmerCircularFile *
emer_circular_file_new (const gchar *path,
                        guint64      max_size,
                        gboolean     reinitialize,
                        GError     **error)
{
  return g_initable_new (EMER_TYPE_CIRCULAR_FILE,
                         NULL /* GCancellable */,
                         error,
                         "path", path,
                         "max-size", max_size,
                         "reinitialize", reinitialize,
                         NULL);
}

/* Appends the given element in-memory only. Use emer_circular_file_save to
 * flush all appended elements. This allows for batching of writes. Note that
 * elements can not be read with emer_circular_file_read until they have been
 * saved. Returns TRUE if the given element was successfully appended and will
 * fit in the space allotted to the circular file. Returns FALSE otherwise.
 */
gboolean
emer_circular_file_append (EmerCircularFile *self,
                           gconstpointer     elem,
                           guint64           elem_size)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  guint64 elem_size_on_disk = sizeof (elem_size) + elem_size;
  guint64 total_size = priv->size + priv->write_buffer->len + elem_size_on_disk;
  if (total_size > priv->max_size)
    return FALSE;

  guint64 little_endian_elem_size = swap_bytes_64_if_big_endian (elem_size);
  g_byte_array_append (priv->write_buffer,
                       (const guint8 *) &little_endian_elem_size,
                       sizeof (little_endian_elem_size));
  g_byte_array_append (priv->write_buffer, elem, elem_size);

  return TRUE;
}

/* Flushes all elements successfully appended via emer_circular_file_append
 * through to the underlying data file. Elements are saved in the same order in
 * which they were appended. Returns TRUE on success and FALSE on error.
 */
gboolean
emer_circular_file_save (EmerCircularFile *self,
                         GError          **error)
{
  g_autoptr(GFileIOStream) file_io_stream = NULL;
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  if (priv->write_buffer->len == 0)
    return TRUE;

  file_io_stream = g_file_open_readwrite (priv->data_file, NULL /* GCancellable */, error);
  if (file_io_stream == NULL)
    return FALSE;

  GIOStream *io_stream = G_IO_STREAM (file_io_stream);
  GOutputStream *output_stream = g_io_stream_get_output_stream (io_stream);
  GSeekable *seekable = G_SEEKABLE (output_stream);

  goffset tail = (priv->head + priv->size) % priv->max_size;
  gsize space_available_at_tail = priv->max_size - tail;
  gsize bytes_tail = MIN(priv->write_buffer->len, space_available_at_tail);
  gsize bytes_start = priv->write_buffer->len - bytes_tail;
  if (bytes_start > 0)
    {
      gboolean seek_succeeded =
        g_seekable_seek (seekable, 0, G_SEEK_SET, NULL /* GCancellable */,
                         error);
      if (!seek_succeeded)
        return FALSE;

      const guint8 *copy_from = priv->write_buffer->data + bytes_tail;
      gboolean write_succeeded =
        g_output_stream_write_all (output_stream,
                                   copy_from,
                                   bytes_start,
                                   NULL /* bytes written */,
                                   NULL /* GCancellable */,
                                   error);
      if (!write_succeeded)
        return FALSE;
    }

  gboolean seek_succeeded =
    g_seekable_seek (seekable, tail, G_SEEK_SET, NULL /* GCancellable */,
                     error);

  if (!seek_succeeded)
    return FALSE;

  gboolean write_succeeded =
    g_output_stream_write_all (output_stream,
                               priv->write_buffer->data,
                               bytes_tail,
                               NULL /* bytes written */,
                               NULL /* GCancellable */,
                               error);
  if (!write_succeeded)
    return FALSE;

  if (!add_to_size (self, priv->write_buffer->len, error))
    return FALSE;

  g_byte_array_unref (priv->write_buffer);
  priv->write_buffer = g_byte_array_new ();
  return TRUE;
}

/* Populates elems with a C array of elements that consume no more than the
 * given number of bytes in total. Note that only the size of the underlying
 * data is taken into consideration, not overhead. Elements are read in the same
 * order in which they were stored; in other words, the circular file is FIFO.
 * Only data that has been successfully saved with emer_circular_file_save will
 * be read. Sets token to an opaque value that may be passed to
 * emer_circular_file_remove to remove the elements that were read in a
 * particular call to emer_circular_file_read. Tokens may not be reused, and any
 * successful call to emer_circular_file_remove invalidates any outstanding
 * tokens. If no elements were read but the read succeeded, then elems is set to
 * NULL. Returns TRUE on success and FALSE on error.
 */
gboolean
emer_circular_file_read (EmerCircularFile *self,
                         GBytes         ***elems,
                         gsize             data_bytes_to_read,
                         gsize            *num_elems,
                         guint64          *token,
                         gboolean         *has_invalid,
                         GError          **error)
{
  g_autoptr(GFileInputStream) file_input_stream = NULL;
  g_autoptr(GPtrArray) elem_array = NULL;
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  *has_invalid = FALSE;

  if (priv->size == 0)
    {
      *elems = NULL;
      *num_elems = 0;
      *token = 0;
      return TRUE;
    }

  file_input_stream = g_file_read (priv->data_file, NULL /* GCancellable */, error);
  if (file_input_stream == NULL)
    return FALSE;

  elem_array = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

  gboolean seek_succeeded =
    g_seekable_seek (G_SEEKABLE (file_input_stream), priv->head, G_SEEK_SET,
                     NULL /* GCancellable */, error);
  if (!seek_succeeded)
    return FALSE;

  guint64 curr_data_bytes = 0;
  guint64 curr_disk_bytes = 0;
  GInputStream *input_stream = G_INPUT_STREAM (file_input_stream);

  while (curr_disk_bytes < priv->size)
    {
      guint64 elem_size;
      if (!read_elem_size (self, input_stream, &elem_size, error))
        return FALSE;

      /* Reading a zero-sized element here means that we have invalid
       * data ahead, in which case we need to update the priv->size
       * pointer so that the next time this is run it does not include
       * the region of invalid data existing after this point.
       */
      if (elem_size == 0)
        {
          g_warning ("Discarding invalid data found after byte %" G_GINT64_FORMAT,
                     (priv->head + curr_disk_bytes) % priv->max_size);
          set_metadata (self, curr_disk_bytes, priv->head, error);
          *has_invalid = TRUE;
          break;
        }

      guint64 next_data_bytes = curr_data_bytes + elem_size;
      if (next_data_bytes > data_bytes_to_read)
        break;

      gboolean append_succeeded =
        append_elem_to_array (self, input_stream, elem_array, elem_size, error);
      if (!append_succeeded)
        return FALSE;

      curr_data_bytes = next_data_bytes;

      /* sizeof (elem_size) gives the number of bytes used to record the
       * element's length on disk.
       */
      curr_disk_bytes += sizeof (elem_size) + elem_size;
    }

  *num_elems = elem_array->len;
  *elems = (GBytes **) g_ptr_array_free (g_steal_pointer (&elem_array), FALSE);
  *token = curr_disk_bytes;
  return TRUE;
}

/* Returns TRUE if there would still be at least one element remaining after a
 * successful call to emer_circular_file_remove with this token. Returns FALSE
 * if a successful call to emer_circular_file_remove with this token would
 * result in all of the elements that are currently in the circular file being
 * removed. Calling emer_circular_file_has_more does not invalidate this or any
 * other token, but passing invalid tokens to emer_circular_file_has_more
 * results in undefined behavior. A token value of 0 may be passed to ascertain
 * whether the circular file is currently empty.
 */
gboolean
emer_circular_file_has_more (EmerCircularFile *self,
                             guint64           token)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  return token < priv->size;
}

/* Removes the elements that were read in the call to emer_circular_file_read
 * that produced the given token. Tokens may not be reused, and any successful
 * call to emer_circular_file_remove invalidates any outstanding tokens. A token
 * value of 0 indicates that no elements should be removed. Returns TRUE on
 * success and FALSE on error.
 */
gboolean
emer_circular_file_remove (EmerCircularFile *self,
                           guint64           token,
                           GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  if (token == 0)
    return TRUE;

  guint64 new_size = priv->size - token;
  goffset new_head = (priv->head + token) % priv->max_size;
  return set_metadata (self, new_size, new_head, error);
}

/* Removes all data stored in the circular file. Does not remove any data that
 * has been appended but not saved. Returns TRUE on success and FALSE on error.
 */
gboolean
emer_circular_file_purge (EmerCircularFile *self,
                          GError          **error)
{
  EmerCircularFilePrivate *priv =
    emer_circular_file_get_instance_private (self);

  return priv->size == 0 ? TRUE : set_metadata (self, 0, 0, error);
}
