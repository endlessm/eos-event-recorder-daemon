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

#include "emer-cache-size-provider.h"

/*
 * The filepath to the metadata file containing the maximum persistent cache
 * size.
 */
#define DEFAULT_CACHE_SIZE_FILE_PATH CONFIG_DIR "cache-size.conf"

/* This is the default maximum cache size in bytes. */
#define DEFAULT_MAX_CACHE_SIZE G_GUINT64_CONSTANT (10000000)

#define CACHE_SIZE_GROUP "persistent_cache_size"
#define MAX_CACHE_SIZE_KEY "maximum"

static void
write_cache_size (GKeyFile              *key_file,
                  const gchar           *path,
                  guint64                max_cache_size)
{
  g_key_file_set_uint64 (key_file, CACHE_SIZE_GROUP,
                         MAX_CACHE_SIZE_KEY, max_cache_size);

  g_autoptr(GError) error = NULL;
  if (!g_key_file_save_to_file (key_file, path, &error));
    {
      g_warning ("Failed to write default cache size file to %s. Error: %s.",
                 path, error->message);
    }
}

/*
 * emer_cache_size_provider_get_max_cache_size:
 * @path: (allow-none): the path to the file where the maximum persistent
 *  cache size is stored.
 *
 * Returns the maximum persistent cache size in bytes. If @path is %NULL, it
 * defaults to DEFAULT_CACHE_SIZE_FILE_PATH. If the underlying configuration
 * file doesn't exist yet, it is created with a default value of
 * DEFAULT_MAX_CACHE_SIZE.
 */
guint64
emer_cache_size_provider_get_max_cache_size (const gchar *path)
{
  g_autoptr(GKeyFile) key_file = g_key_file_new ();
  guint64 cache_size;
  gboolean load_succeeded;
  g_autoptr(GError) error = NULL;

  if (path == NULL)
    path = DEFAULT_CACHE_SIZE_FILE_PATH;

  if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE,
                                  &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        goto handle_failed_read;

      g_clear_error (&error);
      write_cache_size (key_file, path, DEFAULT_MAX_CACHE_SIZE);
    }

  cache_size = g_key_file_get_uint64 (key_file, CACHE_SIZE_GROUP,
                                      MAX_CACHE_SIZE_KEY, &error);
  if (error != NULL)
    goto handle_failed_read;

  return cache_size;

handle_failed_read:
  g_warning ("Failed to read from cache size file. Error: %s.", error->message);
  return 0;
}
