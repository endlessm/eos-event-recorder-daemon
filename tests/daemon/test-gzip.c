/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2015, 2016 Endless Mobile, Inc. */

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
#include <string.h>

static gpointer
gzip_decompress (gconstpointer input_data,
                 gsize         input_length,
                 gsize        *decompressed_length)
{
  GZlibDecompressor *zlib_decompressor =
    g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
  GConverter *converter = G_CONVERTER (zlib_decompressor);

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
      GError *error = NULL;
      GConverterResult conversion_result =
        g_converter_convert (converter,
                             curr_input, bytes_left_in_input,
                             curr_output, bytes_left_in_buffer,
                             conversion_flags,
                             &curr_bytes_read, &curr_bytes_written,
                             &error);

      if (conversion_result == G_CONVERTER_ERROR)
        {
          g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
          g_error_free (error);

          allocated_space *= 2;
          g_byte_array_set_size (byte_array, allocated_space);
          continue;
        }

      total_bytes_read += curr_bytes_read;
      total_bytes_written += curr_bytes_written;

      if (conversion_result == G_CONVERTER_FINISHED)
        break;

      /* Expand the byte array. */
      allocated_space *= 2;
      g_byte_array_set_size (byte_array, allocated_space);
    }

  g_object_unref (zlib_decompressor);
  *decompressed_length = total_bytes_written;
  return g_byte_array_free (byte_array, FALSE);
}

static void
test_gzip_roundtrip (const gchar *input_string)
{
  gsize input_length = strlen (input_string);
  gsize compressed_length;
  GError *error = NULL;
  gpointer compressed_string =
    emer_gzip_compress (input_string, input_length, &compressed_length, &error);
  g_assert_nonnull (compressed_string);
  g_assert_no_error (error);

  gsize decompressed_length;
  gchar *decompressed_string =
    (gchar *) gzip_decompress (compressed_string, compressed_length,
                               &decompressed_length);
  g_free (compressed_string);

  g_assert_cmpmem (input_string, input_length, decompressed_string,
                   decompressed_length);
  g_free (decompressed_string);
}

static void
test_gzip_compress_on_empty_payload (gboolean     *unused,
                                     gconstpointer dont_use_me)
{
  test_gzip_roundtrip ("");
}

static void
test_gzip_compress_on_standard_payload (gboolean     *unused,
                                        gconstpointer dont_use_me)
{
  test_gzip_roundtrip ("How many zips could a gzip zip if a gzip could zip "
                       "zips? A gzip could zip as many zips as a gzip could "
                       "zip if a gzip could zip zips.");
}

static void
test_gzip_compress_on_incompressible_payload (gboolean     *unused,
                                              gconstpointer dont_use_me)
{
  test_gzip_roundtrip ("ô8üO½#Bé_¯ì.¼NÛ½ÊÜÑ\x9côÆoQÉÐàðÒ^P^W£^XxÝ1Z>^?UYô\\à^V¢"
                       "zþzµÿ½ö8\x88\x8f´^L\x81^DÕí¹(^@výþoT³Àû#Ùïq\x89°^MSõ"
                       "\x99\x82müp ¨Ð\x83h\x94)\x88Ó(æ¥Ã'}\x9fæ\x8c^A?OZ\x82#¦"
                       "\x88Ý\n\x8eWï^Q\x88^NãS%\x9d`¥");
}

gint
main (gint                argc,
      const gchar * const argv[])
{
  g_test_init (&argc, (gchar ***) &argv, NULL);

/* We are using a gboolean as a fixture type, but it will go unused. */
#define ADD_GZIP_TEST_FUNC(path, func) \
  g_test_add ((path), gboolean, NULL, NULL, (func), NULL)

  ADD_GZIP_TEST_FUNC ("/gzip/compress-on-empty-payload",
                      test_gzip_compress_on_empty_payload);
  ADD_GZIP_TEST_FUNC ("/gzip/compress-on-standard-payload",
                      test_gzip_compress_on_standard_payload);
  ADD_GZIP_TEST_FUNC ("/gzip/compress-on-incompressible-payload",
                      test_gzip_compress_on_incompressible_payload);

#undef ADD_GZIP_TEST_FUNC

  return g_test_run ();
}
