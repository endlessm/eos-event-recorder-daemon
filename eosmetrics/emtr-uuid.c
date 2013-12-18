/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-uuid-private.h"

#include <glib.h>
#include <uuid/uuid.h>

#define UUID_STRING_LENGTH 37  /* 36 bytes + trailing zero */

/*
 * SECTION:uuid
 * Facility for generating a UUID
 */

/*
 * emtr_uuid_gen:
 *
 * Generate a UUID for use in a fingerprint file identifying the EndlessOS
 * installation.
 *
 * Returns: a string containing the UUID. Free with g_free() when done.
 */
gchar *
emtr_uuid_gen (void)
{
  uuid_t generated_uuid;
  gchar *retval = g_malloc (UUID_STRING_LENGTH);
  uuid_generate (generated_uuid);
  uuid_unparse (generated_uuid, retval);
  return retval;
}
