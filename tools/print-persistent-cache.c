/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014, 2015 Endless Mobile, Inc. */

/* This file is part of eos-event-recorder-daemon.
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

#include "daemon/emer-persistent-cache.h"
#include "shared/metrics-util.h"

#define TMP_DIR_TEMPLATE "persistent-cache-XXXXXX"
#define MAX_CACHE_SIZE G_GUINT64_CONSTANT (92160) // 90 kB
#define CACHE_VERSION_FILENAME "local_version_file"
#define BOOT_OFFSET_UPDATE_INTERVAL (60u * 60u) // 1 hour

#define SINGULAR_OUTPUT_FILE "singulars.printed"
#define AGGREGATE_OUTPUT_FILE "aggregates.printed"
#define SEQUENCE_OUTPUT_FILE "sequences.printed"

typedef struct CopySpec
{
  GFile *source_directory;
  GFile *destination_directory;
} CopySpec;

typedef gboolean (*DirTraversalCallback) (GFile   *file,
                                          gpointer user_data);

static gboolean traverse_file_or_dir (GFile               *file,
                                      gboolean             pre_order,
                                      DirTraversalCallback callback,
                                      gpointer             user_data);

/*
 * Returns a GFile * for the path in the destination directory that corresponds
 * to the given file's path. Assumes the given file resides in the source
 * directory. Free the return value with g_object_unref. Returns NULL on
 * failure.
 */
static GFile *
get_corresponding_file (GFile    *file,
                        CopySpec *copy_spec)
{

  gchar *relative_path =
    g_file_get_relative_path (copy_spec->source_directory, file);
  if (relative_path == NULL)
    {
      g_warning ("Recursive copy failed. Could not get file path in "
                 "destination directory that corresponds to a file path in the "
                 "source directory.");
      return NULL;
    }

  GFile *corresponding_file =
    g_file_resolve_relative_path (copy_spec->destination_directory,
                                  relative_path);
  g_free (relative_path);
  if (corresponding_file == NULL)
    g_warning ("Recursive copy failed. Could not get file in destination "
               "directory that corresponds to a file in the source directory.");
  return corresponding_file;
}

static gboolean
copy_file (GFile *source_file,
           GFile *destination_file)
{
  GError *error = NULL;
  gboolean copy_succeeded =
    g_file_copy (source_file, destination_file, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                 NULL /* GCancellable */, NULL /* GFileProgressCallback */,
                 NULL /* progress_callback_data */, &error);

  if (!copy_succeeded)
    {
      g_warning ("Could not copy a file: %s.", error->message);
      g_error_free (error);
    }

  return copy_succeeded;
}

static gboolean
make_directory (GFile *directory)
{
  GError *error = NULL;
  gboolean make_directory_succeeded =
    g_file_make_directory (directory, NULL /* GCancellable */, &error);

  if (!make_directory_succeeded)
    {
      g_warning ("Could not make an empty directory: %s.", error->message);
      g_error_free (error);
    }

  return make_directory_succeeded;
}

static gboolean
shallow_copy_file_or_dir (GFile *source_file,
                          GFile *destination_file)
{
  GFileType source_file_type =
    g_file_query_file_type (source_file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            NULL /* GCancellable */);
  switch (source_file_type)
    {
    case G_FILE_TYPE_REGULAR:
      return copy_file (source_file, destination_file);

    case G_FILE_TYPE_DIRECTORY:
      return make_directory (destination_file);

    default:
      g_warning ("Could not copy a file because it had unexpected type %d.",
                 source_file_type);
      return FALSE;
    }
}

// Intended for use as a pre-order DirTraversalCallback function.
static gboolean
shallow_copy_file_or_dir_to_dir (GFile    *source_file,
                                 CopySpec *copy_spec)
{
  GFile *destination_file = get_corresponding_file (source_file, copy_spec);
  if (destination_file == NULL)
    return FALSE;

  gboolean copy_succeeded =
    shallow_copy_file_or_dir (source_file, destination_file);

  g_object_unref (destination_file);

  return copy_succeeded;
}

// Intended for use as a post-order DirTraversalCallback function.
static gboolean
delete_file (GFile   *file,
             gpointer unused)
{
  GError *error = NULL;
  gboolean delete_succeeded =
    g_file_delete (file, NULL /* GCancellable */, &error);

  if (!delete_succeeded)
    {
      g_warning ("Could not delete file: %s.", error->message);
      g_error_free (error);
    }

  return delete_succeeded;
}

// Assumes file_enumerator can be queried for G_FILE_ATTRIBUTE_STANDARD_NAME.
static gboolean
traverse_enumerator (GFileEnumerator     *file_enumerator,
                     gboolean             pre_order,
                     DirTraversalCallback callback,
                     gpointer             user_data)
{
  while (TRUE)
    {
      GError *error = NULL;
      GFileInfo *curr_file_info =
        g_file_enumerator_next_file (file_enumerator, NULL /* GCancellable */,
                                     &error);

      if (error != NULL)
        {
          g_warning ("Recursive directory traversal failed. Could not get info "
                     "about a file: %s.", error->message);
          g_error_free (error);
          return FALSE;
        }

      if (curr_file_info == NULL)
        // We reached the end of the file enumerator.
        return TRUE;

      GFile *curr_file =
        g_file_enumerator_get_child (file_enumerator, curr_file_info);
      g_object_unref (curr_file_info);

      gboolean traverse_succeeded =
        traverse_file_or_dir (curr_file, pre_order, callback, user_data);
      g_object_unref (curr_file);

      if (!traverse_succeeded)
        return FALSE;
    }
}

static gboolean
traverse_directory (GFile               *directory,
                    gboolean             pre_order,
                    DirTraversalCallback callback,
                    gpointer             user_data)
{
  GError *error = NULL;
  GFileEnumerator *directory_enumerator =
    g_file_enumerate_children (directory,
                               G_FILE_ATTRIBUTE_STANDARD_NAME,
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               NULL /* GCancellable */,
                               &error);

  if (directory_enumerator == NULL)
    {
      g_warning ("Recursive directory traversal failed. Could not list "
                 "contents of directory: %s.", error->message);
      g_error_free (error);
      return FALSE;
    }

  gboolean traverse_succeeded =
    traverse_enumerator (directory_enumerator, pre_order, callback, user_data);

  g_object_unref (directory_enumerator);
  return traverse_succeeded;
}

static gboolean
traverse_file_or_dir (GFile               *file,
                      gboolean             pre_order,
                      DirTraversalCallback callback,
                      gpointer             user_data)
{
  GFileType file_type =
    g_file_query_file_type (file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            NULL /* GCancellable */);
  switch (file_type)
    {
    case G_FILE_TYPE_REGULAR:
      return callback (file, user_data);

    case G_FILE_TYPE_DIRECTORY:
      {
        gboolean traversal_succeeded = TRUE;
        if (pre_order)
          traversal_succeeded = callback (file, user_data);

        traversal_succeeded = traversal_succeeded &&
          traverse_directory (file, pre_order, callback, user_data);

        if (!pre_order)
          traversal_succeeded = traversal_succeeded &&
            callback (file, user_data);

        return traversal_succeeded;
      }

    default:
      g_warning ("Recursive directory traversal failed. A file had unexpected "
                 "type %d.", file_type);
      char *filename = g_file_get_path (file);
      if (filename != NULL)
        {
          g_warning ("File with unexpected type has name: %s.", filename);
          g_free (filename);
        }
      else
        {
          g_warning ("File with unexpected type has NULL name.");
        }
      return FALSE;
    }
}

// Assumes the destination directory already exists.
static gboolean
copy_directory (GFile *source_directory,
                GFile *destination_directory)
{
  CopySpec copy_spec = { source_directory, destination_directory };
  return traverse_directory (source_directory, TRUE,
                             (DirTraversalCallback)
                             shallow_copy_file_or_dir_to_dir,
                             &copy_spec);

}

static gboolean
delete_directory (GFile *directory)
{
  return traverse_file_or_dir (directory, FALSE,
                               (DirTraversalCallback) delete_file,
                               NULL /* user_data */);
}

/*
 * Recursively copies the directory at the given path to a new temporary
 * directory, which is returned. Free the return value with g_object_unref.
 * Returns NULL on failure.
 */
static GFile *
copy_persistent_cache (const gchar *persistent_cache_path)
{
  GError *error = NULL;
  gchar *tmp_dir_name = g_dir_make_tmp (TMP_DIR_TEMPLATE, &error);
  if (tmp_dir_name == NULL)
    {
      g_warning ("Could not create temporary directory from template %s: %s.",
                 TMP_DIR_TEMPLATE, error->message);
      g_error_free (error);
      return NULL;
    }

  GFile *tmp_dir = g_file_new_for_path (tmp_dir_name);
  g_free (tmp_dir_name);

  GFile *persistent_cache_dir = g_file_new_for_path (persistent_cache_path);

  gboolean copy_succeeded = copy_directory (persistent_cache_dir, tmp_dir);

  g_object_unref (persistent_cache_dir);

  if (!copy_succeeded)
    {
      g_object_unref (tmp_dir);
      return NULL;
    }

  return tmp_dir;
}

static gboolean
print_variant_to_file (GVariant      *variant,
                       GOutputStream *output_stream)
{
  gchar *variant_str = g_variant_print (variant, TRUE);
  gsize variant_str_length = strlen (variant_str);
  gsize bytes_written;
  GError *error = NULL;
  gboolean write_succeeded =
    g_output_stream_write_all (output_stream, variant_str, variant_str_length,
                               &bytes_written, NULL /* GCancellable */, &error);
  g_free (variant_str);

  if (!write_succeeded)
    {
      g_warning ("Failed to print event to file: %s.", error->message);
      g_error_free (error);
    }

  return write_succeeded;
}

static gboolean
print_variant_array_to_file (GVariant **variant_array,
                             gchar     *filename)
{
  GFile *file = g_file_new_for_path (filename);
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

  GOutputStream *output_stream = G_OUTPUT_STREAM (file_output_stream);

  gboolean write_succeeded = TRUE;
  for (gint i = 0; variant_array[i] != NULL; i++)
    {
      write_succeeded =
        print_variant_to_file (variant_array[i], output_stream);
      if (!write_succeeded)
        break;
    }

  g_object_unref (file_output_stream);
  return write_succeeded;
}

static EmerPersistentCache *
make_persistent_cache (GFile *directory)
{
  GError *error = NULL;

  gchar *directory_path = g_file_get_path (directory);
  if (directory_path == NULL)
    {
      g_warning ("Could not get path of directory.");
      return NULL;
    }

  gchar *directory_path_with_slash = g_strconcat (directory_path, "/", NULL);
  g_free (directory_path);

  EmerBootIdProvider *boot_id_provider = emer_boot_id_provider_new ();
  gchar *cache_version_path =
    g_strconcat (directory_path_with_slash, CACHE_VERSION_FILENAME, NULL);
  EmerCacheVersionProvider *cache_version_provider =
    emer_cache_version_provider_new_full (cache_version_path);
  g_free (cache_version_path);

  EmerPersistentCache *persistent_cache =
    emer_persistent_cache_new_full (NULL /* GCancellable */, &error,
                                    directory_path_with_slash, MAX_CACHE_SIZE,
                                    boot_id_provider, cache_version_provider,
                                    BOOT_OFFSET_UPDATE_INTERVAL);
  g_free (directory_path_with_slash);

  if (persistent_cache == NULL)
    g_error_free (error);

  return persistent_cache;
}

int
main (int   argc,
      char *argv[])
{
  gchar *persistent_cache_path = NULL;
  GOptionEntry options[] =
  {
    {
      // FIXME: Replace 0 with G_OPTION_FLAG_NONE once GLib 2.42 is available on
      // Endless OS.
      "persistent-cache-path", 'p', 0 /* G_OPTION_FLAG_NONE */,
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

  GFile *tmp_dir = copy_persistent_cache (persistent_cache_path);
  g_free (persistent_cache_path);

  if (tmp_dir == NULL)
    return EXIT_FAILURE;

  EmerPersistentCache *persistent_cache = make_persistent_cache (tmp_dir);
  if (persistent_cache == NULL)
    {
      g_object_unref (tmp_dir);
      return EXIT_FAILURE;
    }

  GVariant **singulars, **aggregates, **sequences;
  gboolean drain_succeeded =
    emer_persistent_cache_drain_metrics (persistent_cache, &singulars,
                                         &aggregates, &sequences,
                                         MAX_CACHE_SIZE);

  g_object_unref (persistent_cache);

  if (!drain_succeeded)
    {
      g_object_unref (tmp_dir);
      return EXIT_FAILURE;
    }

  gboolean print_succeeded =
    print_variant_array_to_file (singulars, SINGULAR_OUTPUT_FILE);
  free_variant_array (singulars);

  print_succeeded = print_succeeded &&
    print_variant_array_to_file (aggregates, AGGREGATE_OUTPUT_FILE);
  free_variant_array (aggregates);

  print_succeeded = print_succeeded &&
    print_variant_array_to_file (sequences, SEQUENCE_OUTPUT_FILE);
  free_variant_array (sequences);

  gboolean delete_succeeded = delete_directory (tmp_dir);
  g_object_unref (tmp_dir);

  return print_succeeded && delete_succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
