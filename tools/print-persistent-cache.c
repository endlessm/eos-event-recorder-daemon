/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 - 2016 Endless Mobile, Inc. */

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

#include <gio/gio.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "emer-cache-size-provider.h"
#include "emer-persistent-cache.h"
#include "shared/metrics-util.h"

#define TMP_DIR_TEMPLATE "persistent-cache-XXXXXX"

#define CACHE_VERSION_FILENAME "local_version_file"
#define BOOT_OFFSET_UPDATE_INTERVAL (60u * 60u) // 1 hour

#define OUTPUT_FILE "variants.txt"

static gboolean
print_variants_to_file (GVariant   **variants,
                        gsize        num_variants,
                        const gchar *path)
{
  GFile *file = g_file_new_for_path (path);
  GError *error = NULL;
  GFileOutputStream *file_output_stream =
    g_file_append_to (file, G_FILE_CREATE_NONE, NULL /* GCancellable */,
                      &error);
  g_object_unref (file);

  if (file_output_stream == NULL)
    {
      g_warning ("Could not open file output stream for appending: %s.",
                 error->message);
      g_error_free (error);
      return FALSE;
    }

  if (num_variants == 0)
    {
      g_object_unref (file_output_stream);
      return TRUE;
    }

  GByteArray *byte_array = g_byte_array_new ();
  for (gsize i = 0; i < num_variants; i++)
    {
      gchar *variant_str = g_variant_print (variants[i], TRUE);
      gsize variant_str_length = strlen (variant_str);
      g_byte_array_append (byte_array, (const guint8 *) variant_str,
                           variant_str_length);
      g_byte_array_append (byte_array, (const guint8 *) "\n", 1);
      g_free (variant_str);
    }

  GOutputStream *output_stream = G_OUTPUT_STREAM (file_output_stream);

  gboolean write_succeeded =
    g_output_stream_write_all (output_stream, byte_array->data, byte_array->len,
                               NULL /* bytes written */,
                               NULL /* GCancellable */, &error);

  g_object_unref (file_output_stream);
  g_byte_array_unref (byte_array);

  if (!write_succeeded)
    {
      g_warning ("Failed to print variants to file: %s.", error->message);
      g_error_free (error);
    }

  return write_succeeded;
}

static EmerPersistentCache *
make_persistent_cache (const gchar *directory)
{
  GError *error = NULL;
  guint64 max_cache_size =
    emer_cache_size_provider_get_max_cache_size (NULL);
  EmerPersistentCache *persistent_cache =
    emer_persistent_cache_new (directory, max_cache_size, FALSE, &error);

  if (persistent_cache == NULL)
    {
      g_warning ("Could not create persistent cache. Error: %s.",
                 error->message);
      g_error_free (error);
    }

  return persistent_cache;
}

gint
main (gint   argc,
      gchar *argv[])
{
  gchar *persistent_cache_path = NULL;
  GOptionEntry options[] =
  {
    {
      "persistent-cache-path", 'p', G_OPTION_FLAG_NONE,
      G_OPTION_ARG_FILENAME, &persistent_cache_path,
      "The filepath to the persistent cache to print.",
      NULL /* argument description */
    },
    {
      NULL
    }
  };

  GOptionContext *option_context =
    g_option_context_new ("Log the contents of a persistent cache in a "
                          "human-readable format.");

  g_option_context_add_main_entries (option_context, options, NULL);

  GError *error = NULL;
  gboolean parse_succeeded =
    g_option_context_parse (option_context, &argc, &argv, &error);
  g_option_context_free (option_context);

  if (!parse_succeeded)
    {
      g_warning ("Could not parse arguments: %s.", error->message);
      g_error_free (error);
      return EXIT_FAILURE;
    }

  if (argc != 1 || persistent_cache_path == NULL)
    {
      g_free (persistent_cache_path);
      g_warning ("Invalid parameter(s). Usage: %s "
                 "--persistent-cache-path=<filepath>.", argv[0]);
      return EXIT_FAILURE;
    }

  EmerPersistentCache *persistent_cache =
    make_persistent_cache (persistent_cache_path);
  g_free (persistent_cache_path);
  if (persistent_cache == NULL)
    return EXIT_FAILURE;

  GVariant **variants;
  gsize num_variants;
  guint64 token;
  gboolean has_invalid;
  gboolean read_succeeded =
    emer_persistent_cache_read (persistent_cache, &variants, G_MAXSIZE,
                                &num_variants, &token, &has_invalid, &error);
  g_object_unref (persistent_cache);

  if (!read_succeeded)
    {
      g_warning ("Could not read from persistent cache. Error: %s.",
                 error->message);
      g_error_free (error);
      return EXIT_FAILURE;
    }

  gboolean print_succeeded =
    print_variants_to_file (variants, num_variants, OUTPUT_FILE);
  destroy_variants (variants, num_variants);

  return print_succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
