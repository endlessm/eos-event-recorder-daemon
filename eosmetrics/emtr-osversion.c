/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-osversion-private.h"

#include <string.h>
#include <glib.h>

/*
 * SECTION:osversion
 * Facility for reading the EndlessOS system version
 */

struct OsVersionParseResult {
  gchar *major;
  gchar *minor;
  gchar *micro;
};

/* Callback function for when the GMarkup parser encounters text inside an
element */
static void
osversion_parser_text (GMarkupParseContext *ctxt,
                       const gchar         *text,
                       gsize                length,
                       gpointer             data,
                       GError             **error)
{
  struct OsVersionParseResult *result = (struct OsVersionParseResult *)data;
  const GSList *stack = g_markup_parse_context_get_element_stack (ctxt);

  /* stack is not NULL, because at least one element is guaranteed to be open
  in this callback */
  g_assert (stack != NULL);

  /* Verify that the current element is contained within an <endlessos-version>
  element */
  const GSList *parent_element = stack->next;
  if (parent_element == NULL
      || strcmp (parent_element->data, "endlessos-version") != 0)
    return;

  if (strcmp (stack->data, "platform") == 0)
    {
      result->major = g_strndup (text, length);
      return;
    }
  if (strcmp (stack->data, "minor") == 0)
    {
      result->minor = g_strndup (text, length);
      return;
    }
  if (strcmp (stack->data, "micro") == 0)
    {
      result->micro = g_strndup (text, length);
      return;
    }
}

/*
 * emtr_get_os_version:
 *
 * Retrieves the Endless OS version as a string.
 *
 * Returns: (transfer full) (allow-none): a string such as "2.2.0", or %NULL if
 * an error occurred.
 * Free this string with g_free() when done.
 */
gchar *
emtr_get_os_version (void)
{
  const gchar *version_filename = DATADIR "/EndlessOS/endlessos-version.xml";
  gchar *version_file_contents;
  gsize contents_length;
  GError *error = NULL;

  /* For testing only; setting this environment variable anywhere else is a
  programmer error */
  const gchar *mock_version_filename =
    g_getenv ("_MOCK_ENDLESSOS_VERSION_FILE");
  if (mock_version_filename != NULL)
    version_filename = mock_version_filename;

  if (!g_file_get_contents (version_filename, &version_file_contents,
                            &contents_length, &error))
    {
      g_critical ("Could not read version file '%s': %s", version_filename,
                  error->message);
      g_error_free (error);
      goto fail;
    }

  GMarkupParser parser = {
    .start_element = NULL,
    .end_element = NULL,
    .text = osversion_parser_text,
    .passthrough = NULL,
    .error = NULL
  };
  struct OsVersionParseResult *result =
    g_slice_new0 (struct OsVersionParseResult);
  GMarkupParseContext *ctxt = g_markup_parse_context_new (&parser, 0,
                                                          result, NULL);
  if (!g_markup_parse_context_parse (ctxt, version_file_contents,
                                     contents_length, &error))
    {
      g_critical ("Problem reading version file '%s': %s", version_filename,
                  error->message);
      g_error_free (error);
      goto fail3;
    }
  if (!g_markup_parse_context_end_parse (ctxt, &error))
    {
      g_critical ("Version file '%s' was incomplete: %s", version_filename,
                  error->message);
      g_error_free (error);
      goto fail3;
    }
  g_markup_parse_context_free (ctxt);
  g_free (version_file_contents);

  if (result->major == NULL || result->minor == NULL || result->micro == NULL)
    {
      g_critical ("Version file '%s' did not contain a version number",
                  version_filename);
      goto fail2;
    }

  gchar *retval = g_strconcat (result->major, ".",
                               result->minor, ".",
                               result->micro, NULL);

  g_slice_free (struct OsVersionParseResult, result);

  return retval;

fail3:
  g_markup_parse_context_free (ctxt);
  g_free (version_file_contents);
fail2:
  g_slice_free (struct OsVersionParseResult, result);
fail:
  return NULL;
}
