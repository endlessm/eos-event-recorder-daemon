/* Copyright 2014 Endless Mobile, Inc. */

#include "eosmetrics.h"
#include <glib.h>
#include <uuid/uuid.h>

/**
 * emtr_event_id_to_name:
 * @event_id: The hexadecimal event ID to translate.
 * @event_name: (out): The human readable name of the event.
 *
 * Takes an event ID in hexadecimal format, converts it to a human-readable
 * format, and populates the event_name out parameter with the readable name.
 * If successful, will return %TRUE. If the given event ID is not a valid UUID
 * or simply not included in this function, this will return %FALSE and the
 * contents of the out parameter will be set to a message indicating the error.
 *
 * Returns: A gboolean indicating success or failure.
 */
gboolean
emtr_event_id_to_name (const gchar  *event_id,
                       const gchar **event_name)
{
  static const gchar * const event_descriptions[] = {
    EMTR_EVENT_USER_IS_LOGGED_IN, "User is logged in",
    EMTR_EVENT_NETWORK_STATUS_CHANGED, "Network status changed",
    EMTR_EVENT_SHELL_APP_IS_OPEN, "Shell app is open",
    EMTR_EVENT_SOCIAL_BAR_IS_VISIBLE, "Social bar is visible",
    EMTR_EVENT_SHELL_APP_ADDED, "Shell app added",
    EMTR_EVENT_SHELL_APP_REMOVED, "Shell app removed",
    NULL, NULL
  };

  /* Will catch invalid UUIDs, even if they match one in the event_descriptions
     array, in the which case someone made a mistake in the header file's
     definitions by adding an invalid UUID there. */
  uuid_t uuid_out;
  if (uuid_parse (event_id, uuid_out) != 0)
    {
      g_critical ("Invalid event ID given: %s! This was not a valid UUID, and "
                  "was probably generated in error.", event_id);
      *event_name = "(invalid event)";
      return FALSE;
    }

  for (gint i = 0; event_descriptions[i] != NULL; i += 2)
    {
      if (g_strcmp0 (event_id, event_descriptions[i]) == 0)
      {
        *event_name = event_descriptions[i + 1];
        return TRUE;
      }
    }

  /* If we can't find the UUID among our registered events, it probably just
     hasn't been added to this function yet. */
  g_warning ("Unknown ID Given: %s. The translation function %s may need to be "
             "updated.", event_id, G_STRFUNC);
  *event_name = "(unknown event)";
  return FALSE;
}
