/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include <glib.h>

#include <eosmetrics/emtr-mac-private.h>
#include "run-tests.h"

static void
test_mac_returns_the_same_thing_on_subsequent_calls (void)
{
  gint64 mac1 = emtr_mac_gen ();
  gint64 mac2 = emtr_mac_gen ();
  g_assert (mac1 == mac2);
}

void
add_mac_tests (void)
{
  g_test_add_func ("/mac/returns-the-same-thing-on-subsequent-calls",
                   test_mac_returns_the_same_thing_on_subsequent_calls);
}
