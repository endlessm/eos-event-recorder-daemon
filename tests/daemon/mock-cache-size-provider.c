/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2017 Endless Mobile, Inc. */

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
#include "emer-cache-size-provider.h"

/* This is the default maximum cache size in bytes. */
#define DEFAULT_MAX_CACHE_SIZE G_GUINT64_CONSTANT (10000000)

guint64
emer_cache_size_provider_get_max_cache_size (const gchar *path)
{
  /* This mock should only be used by test-daemon.c when testing emer-daemon's
   * logic to construct the persistent cache itself (as used in production). In
   * that case, the daemon should attempt to load the cache size from the
   * default path, which is expressed as NULL. Fail any test which accidentally
   * attempts to use this mock to read a real file.
   */
  g_assert_cmpstr (path, ==, NULL);

  return DEFAULT_MAX_CACHE_SIZE;
}
