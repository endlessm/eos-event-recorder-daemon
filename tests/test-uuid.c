/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <string.h>
#include <glib.h>

#include <eosmetrics/emtr-uuid-private.h>
#include "run-tests.h"

static void
test_uuid_returns_something_reasonable (void)
{
  gchar *uuid = emtr_uuid_gen ();
  g_assert (uuid != NULL);
  g_assert_cmpuint (strspn (uuid, "0123456789ABCDEFabcdef-"), ==,
                    strlen (uuid));
  g_free (uuid);
}

static void
test_uuid_returns_two_different_things_on_subsequent_calls (void)
{
  gchar *uuid1 = emtr_uuid_gen ();
  gchar *uuid2 = emtr_uuid_gen ();
  g_assert_cmpstr (uuid1, !=, uuid2);
  g_free (uuid1);
  g_free (uuid2);
}

void
add_uuid_tests (void)
{
  g_test_add_func ("/uuid/returns-something-reasonable",
                   test_uuid_returns_something_reasonable);
  g_test_add_func ("/uuid/returns-two-different-things-on-subsequent-calls",
                   test_uuid_returns_two_different_things_on_subsequent_calls);
}
