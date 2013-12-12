/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <glib.h>
#include <gio/gio.h>

#include <eosmetrics/emtr-util.h>
#include <eosmetrics/emtr-util-private.h>
#include "run-tests.h"

static void
test_util_data_dir_not_null (void)
{
  GFile *file = emtr_get_data_dir ();
  g_assert (file != NULL);
  g_object_unref (file);
}

static void
test_util_storage_dir_not_null (void)
{
  GFile *file = emtr_get_default_storage_dir ();
  g_assert (file != NULL);
  g_object_unref (file);
}

void
add_util_tests (void)
{
  g_test_add_func ("/util/data-dir-not-null",
                   test_util_data_dir_not_null);
  g_test_add_func ("/util/storage-dir-not-null",
                   test_util_storage_dir_not_null);
}
