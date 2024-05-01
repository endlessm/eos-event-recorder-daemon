/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2024 Endless OS Foundation LLC */

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

#include "emer-aggregate-tally.h"

/* Keep in sync with format string in print_event() */
static const char *HEADER =
  "Date       "
  "   User ID "
  "Event ID                             "
  "   Counter "
  "Payload\n";

static void
print_event (guint32     unix_user_id,
             uuid_t      event_id,
             GVariant   *payload,
             guint32     counter,
             const char *date,
             gpointer    user_data)
{
  char event_id_str[UUID_STR_LEN];
  g_autofree gchar *payload_str = payload ? g_variant_print (payload, TRUE) : NULL;

  uuid_unparse (event_id, event_id_str);

  /* Keep in sync with HEADER */
  g_print ("%-10s "
           "%10" G_GUINT32_FORMAT " "
           "%s "
           "%10" G_GUINT32_FORMAT " "
           "%s\n",
           date,
           unix_user_id,
           event_id_str,
           counter,
           payload_str ?: "");
}

static gboolean
print_events (EmerAggregateTally  *tally,
              EmerTallyType        tally_type,
              GError             **error)
{
  return emer_aggregate_tally_iter (tally,
                                    tally_type,
                                    NULL,
                                    EMER_TALLY_ITER_FLAG_DEFAULT,
                                    print_event,
                                    NULL,
                                    error);
}

gint
main (gint   argc,
      gchar *argv[])
{
  g_autofree gchar *persistent_cache_directory = NULL;
  g_autoptr(EmerAggregateTally) tally = NULL;
  g_autoptr(GError) error = NULL;
  GOptionEntry options[] =
  {
    {
      "database-directory", 'd', G_OPTION_FLAG_NONE,
      G_OPTION_ARG_FILENAME, &persistent_cache_directory,
      "The directory holding metrics.db (default: " PERSISTENT_CACHE_DIR ")",
      NULL /* argument description */
    },
    {
      0
    }
  };

  GOptionContext *option_context =
    g_option_context_new ("Log buffered aggregate events in a human-readable format.");

  g_option_context_add_main_entries (option_context, options, NULL);

  gboolean parse_succeeded =
    g_option_context_parse (option_context, &argc, &argv, &error);
  g_option_context_free (option_context);

  if (!parse_succeeded)
    {
      g_warning ("Could not parse arguments: %s.", error->message);
      return EXIT_FAILURE;
    }

  if (persistent_cache_directory == NULL)
    persistent_cache_directory = g_strdup (PERSISTENT_CACHE_DIR);

  const gboolean read_only = TRUE;
  tally = emer_aggregate_tally_new (persistent_cache_directory, read_only, &error);
  if (tally == NULL)
    {
      g_printerr ("Could not open database: %s\n", error->message);
      return EXIT_FAILURE;
    }

  g_print ("%s", HEADER);
  if (!print_events (tally, EMER_TALLY_DAILY_EVENTS, &error) ||
      !print_events (tally, EMER_TALLY_MONTHLY_EVENTS, &error))
    {
      g_printerr ("Error while listing events: %s\n", error->message);
      return EXIT_FAILURE;
    }

  return 0;
}
