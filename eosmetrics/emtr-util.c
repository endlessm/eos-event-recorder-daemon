/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-util-private.h"

#include <glib.h>
#include <gio/gio.h>

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
