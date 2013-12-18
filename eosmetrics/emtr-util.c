/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-util.h"
#include "emtr-util-private.h"

#include <glib.h>
#include <gio/gio.h>

/**
 * SECTION:emtr-util
 * @title: General
 * @short_description: General metrics functions
 * @include: eosmetrics/eosmetrics.h
 *
 * These are general functions available in the metrics kit.
 *
 * Use emtr_get_default_storage_dir() to get a handle to the directory where
 * metrics data is queued up for sending, in case you want to examine the queue
 * yourself.
 */

/*
 * emtr_get_data_dir:
 *
 * Library-private function.
 *
 * Returns: (transfer full): a #GFile pointing to $XDG_DATA_HOME/eosmetrics
 */
GFile *
emtr_get_data_dir (void)
{
  const gchar *user = g_get_user_data_dir ();
  GFile *user_dir = g_file_new_for_path (user);
  GFile *retval = g_file_get_child (user_dir, "eosmetrics");
  g_object_unref (user_dir);
  return retval;
}

/**
 * emtr_get_default_storage_dir:
 *
 * Retrieves the default directory where metrics data is queued up for sending
 * if it couldn't be sent immediately.
 * Usually you won't need to use this function unless you are doing something
 * tricky like inserting items into the queue yourself.
 *
 * Returns: (transfer full): a #GFile pointing to the directory.
 * Free with g_object_unref() when done.
 */
GFile *
emtr_get_default_storage_dir (void)
{
  GFile *data_dir = emtr_get_data_dir ();
  GFile *retval = g_file_get_child (data_dir, "storage");
  g_object_unref (data_dir);
  return retval;
}
