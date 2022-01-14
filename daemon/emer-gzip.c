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
#include "emer-gzip.h"

#include <gio/gio.h>
#include <glib.h>

/* 9 is the highest compression level, meaning it typically achieves the best
 * compression ratio but takes the longest time to run.
 */
#define COMPRESSION_LEVEL 9

/*
 * SECTION:emer-gzip
 * @title: gzip
 * @short_description: Compresses data using the gzip algorithm.
 *
 * Provides a simplified interface to GZlibCompressor that only supports
 * compression level 9, the gzip algorithm, and non-streaming compression.
 */

/*
 * emer_gzip_compress:
 * @input_data: the data to compress.
 * @input_length: the length of the data to compress in bytes.
 * @compressed_length: (out): the length of the compressed data.
 * @error: (out) (optional): if compression failed, error will be set to a GError
 * describing the failure; otherwise it won't be modified. Pass NULL to ignore
 * this value.
 *
 * Compresses input_data with the gzip algorithm at compression level 9. Returns
 * NULL and sets error if compression fails. Sets compressed_length to the
 * length of the compressed data in bytes.
 *
 * Returns: the compressed data or NULL if compression fails. Free with g_free.
 */
gpointer
emer_gzip_compress (gconstpointer input_data,
                    gsize         input_length,
                    gsize        *compressed_length,
                    GError      **error)
{
  GZlibCompressor *zlib_compressor =
    g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, COMPRESSION_LEVEL);
  GConverter *converter = G_CONVERTER (zlib_compressor);

  gsize allocated_space = input_length + 1;
  GByteArray *byte_array = g_byte_array_sized_new (allocated_space);
  gsize total_bytes_read = 0;
  gsize total_bytes_written = 0;
  while (TRUE)
    {
      gsize bytes_left_in_buffer = allocated_space - total_bytes_written;
      if (bytes_left_in_buffer == 0)
        {
          allocated_space *= 2;
          g_byte_array_set_size (byte_array, allocated_space);
          continue;
        }

      gsize bytes_left_in_input = input_length - total_bytes_read;
      GConverterFlags conversion_flags = bytes_left_in_input > 0 ?
        G_CONVERTER_NO_FLAGS : G_CONVERTER_INPUT_AT_END;

      guint8 *curr_output = byte_array->data + total_bytes_written;
      const guint8 *curr_input =
        ((const guint8 *) input_data) + total_bytes_read;

      gsize curr_bytes_written, curr_bytes_read;
      GError *local_error = NULL;
      GConverterResult conversion_result =
        g_converter_convert (converter,
                             curr_input, bytes_left_in_input,
                             curr_output, bytes_left_in_buffer,
                             conversion_flags,
                             &curr_bytes_read, &curr_bytes_written,
                             &local_error);

      if (conversion_result == G_CONVERTER_ERROR)
        {
          if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NO_SPACE))
            {
              g_error_free (local_error);

              allocated_space *= 2;
              g_byte_array_set_size (byte_array, allocated_space);
              continue;
            }

          g_object_unref (zlib_compressor);
          g_byte_array_free (byte_array, TRUE);
          g_propagate_error (error, local_error);
          return NULL;
        }

      total_bytes_read += curr_bytes_read;
      total_bytes_written += curr_bytes_written;

      if (conversion_result == G_CONVERTER_FINISHED)
        break;

      /* Expand the byte array. */
      allocated_space *= 2;
      g_byte_array_set_size (byte_array, allocated_space);
    }

  g_object_unref (zlib_compressor);
  gpointer compressed_data = g_memdup (byte_array->data, total_bytes_written);
  g_byte_array_free (byte_array, TRUE);
  *compressed_length = total_bytes_written;
  return compressed_data;
}
