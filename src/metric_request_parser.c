#include <ctype.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib-object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <uuid/uuid.h>


/**
 * There are 16 bytes in a UUID, which are equivalent to 32 characters in
 * a hexadecimal string, which are represented by 32 bytes. The string also
 * contains an additional 4 dash '-' characters, which are represented by
 * another 4 bytes. The string then needs one more byte for the null-terminating
 * byte. The total length of the UUID char buffer is 32 + 4 + 1 = 37 bytes.
 */
#define UUID_STRING_LENGTH 37

/**
 * Returns a gchar string that is the parsed UUID of the given GVariant.
 * It is the responsibility of the caller to free the returned string.
 */
static gchar *
get_uuid_from_tuple (GVariant *variant, gsize index)
{
  GVariantIter *uuid_iterator;
  g_variant_get_child (variant, index, "ay", &uuid_iterator);
  uuid_t uuid;
  for (gint i = 0; i < sizeof (uuid_t); ++i)
    {
      g_assert (g_variant_iter_next (uuid_iterator, "y",
                                     &uuid[i]));
    }
  g_assert (g_variant_iter_next_value (uuid_iterator) == NULL);
  g_variant_iter_free (uuid_iterator);
  gchar unparsed_uuid[UUID_STRING_LENGTH];
  uuid_unparse (uuid, unparsed_uuid);
  return g_strdup (unparsed_uuid);
}

static gchar *
get_maybe_variant_from_tuple (GVariant *variant, gsize index)
{
  GVariant *maybe_variant = g_variant_get_child_value (variant, index);
  gchar *maybe_variant_str = g_variant_print (maybe_variant, TRUE);
  return maybe_variant_str;
}

static void
fprintf_events_from_tuple (FILE *output_stream, GVariant *variant, gsize index)
{
  GVariantIter *event_iterator;
  g_variant_get_child (variant, index, "a(uayxmv)", &event_iterator);

  fprintf (output_stream, "Events: [");

  GVariant *event;
  while ((event = g_variant_iter_next_value (event_iterator)) != NULL)
    {
      guint32 user_id;
      g_variant_get_child (event, 0, "u", &user_id);
      char *event_type = get_uuid_from_tuple (event, 1);
      gint64 relative_time;
      g_variant_get_child (event, 2, "x", &relative_time);
      gchar *auxiliary_payload = get_maybe_variant_from_tuple (event, 3);
      g_variant_unref (event);
      fprintf (output_stream, "(Event type: %s, Relative time: %"G_GINT64_FORMAT", "
              "Auxiliary payload: %s), ", event_type, relative_time,
              auxiliary_payload);
      g_free (event_type);
      g_free (auxiliary_payload);
    }

  g_variant_iter_free (event_iterator);
  fprintf (output_stream, "]\n");
}

static void
fprintf_aggregates_from_tuple (FILE *output_stream, GVariant *variant, gsize index)
{
  GVariantIter *aggregate_iterator;
  g_variant_get_child (variant, index, "a(uayxxmv)", &aggregate_iterator);

  fprintf (output_stream, "Aggregates: [");

  GVariant *aggregate;
  while ((aggregate = g_variant_iter_next_value (aggregate_iterator)) != NULL)
    {
      guint32 user_id;
      g_variant_get_child (aggregate, 0, "u", &user_id);
      char *event_type = get_uuid_from_tuple (aggregate, 1);
      gint64 relative_time;
      g_variant_get_child (aggregate, 2, "x", &relative_time);
      gint64 event_count;
      g_variant_get_child (aggregate, 3, "x", &event_count);
      gchar *auxiliary_payload = get_maybe_variant_from_tuple (aggregate, 4);
      g_variant_unref (aggregate);
      fprintf (output_stream, "(Event type: %s, Relative time: %"G_GINT64_FORMAT", "
               "Event count: %"G_GINT64_FORMAT", "
              "Auxiliary payload: %s), ", event_type, relative_time,
              event_count, auxiliary_payload);
      g_free (event_type);
      g_free (auxiliary_payload);
    }

  g_variant_iter_free (aggregate_iterator);
  fprintf (output_stream, "]\n");
}

static void
fprintf_event_sequences_from_tuple (FILE *output_stream, GVariant *variant, gsize index)
{
  GVariantIter *event_sequence_iterator;
  g_variant_get_child (variant, index, "a(uaya(xmv))", &event_sequence_iterator);

  fprintf (output_stream, "Event sequences: [");

  GVariant *event_sequence;
  while ((event_sequence = g_variant_iter_next_value (event_sequence_iterator)) != NULL)
    {
      guint32 user_id;
      g_variant_get_child (event_sequence, 0, "u", &user_id);
      char *event_type = get_uuid_from_tuple (event_sequence, 1);
      fprintf (output_stream, "(Event type: %s, [", event_type);
      g_free (event_type);
      GVariantIter *event_value_iterator;
      g_variant_get_child (event_sequence, 2, "a(xmv)", &event_value_iterator);

      GVariant *event_value;
      while ((event_value = g_variant_iter_next_value (event_value_iterator)) != NULL)
        {
          gint64 relative_time;
          g_variant_get_child (event_value, 0, "x", &relative_time);
          gchar *auxiliary_payload = get_maybe_variant_from_tuple (event_value, 1);
          g_variant_unref (event_value);
          fprintf (output_stream, "(Relative time: %"G_GINT64_FORMAT", Auxiliary payload: %s), ",
                   relative_time, auxiliary_payload);
          g_free (auxiliary_payload);
        }
      g_variant_iter_free (event_value_iterator);
      fprintf (output_stream, "]), ");
    }

  g_variant_iter_free (event_sequence_iterator);
  fprintf (output_stream, "]\n");
}

static GVariant *
deserialize_metrics (gconstpointer serialized_metrics,
                     gsize         length)
{
  GVariant *metrics_variant = g_variant_new_from_data (
                              G_VARIANT_TYPE ("(ixxaysa(uayxmv)a(uayxxmv)a(uaya(xmv)))"),
                             serialized_metrics, length, FALSE, NULL, NULL);

  GVariant *native_endian_metrics_variant = metrics_variant;
  if (G_BYTE_ORDER == G_LITTLE_ENDIAN)
    {
      native_endian_metrics_variant = g_variant_byteswap (metrics_variant);
      g_variant_unref (metrics_variant);
    }
  else
    {
      g_assert (G_BYTE_ORDER == G_BIG_ENDIAN);
    }
  return native_endian_metrics_variant;
}

static void
fprintf_serialized_metrics (FILE *output_stream,
                            guchar serialized_metrics[],
                            size_t length)
{
  GVariant *metrics_variant = deserialize_metrics (serialized_metrics,
                                                   length);

  gint32 client_version;
  g_variant_get_child (metrics_variant, 0, "i", &client_version);
  fprintf (output_stream, "Client version: %d\n", client_version);

  gint64 relative_time;
  g_variant_get_child (metrics_variant, 1, "x", &relative_time);
  fprintf (output_stream, "Relative time: %"G_GINT64_FORMAT"\n", relative_time);

  gint64 absolute_time;
  g_variant_get_child (metrics_variant, 2, "x", &absolute_time);
  fprintf (output_stream, "Absolute time: %"G_GINT64_FORMAT"\n", absolute_time);

  char *client_id = get_uuid_from_tuple (metrics_variant, 3);
  fprintf (output_stream, "Client ID: %s\n", client_id);
  g_free (client_id);

  gchar *environment;
  g_variant_get_child (metrics_variant, 4, "s", &environment);
  fprintf (output_stream, "Environment: %s\n", environment);
  g_free (environment);

  fprintf_events_from_tuple (output_stream, metrics_variant, 5);

  fprintf_aggregates_from_tuple (output_stream, metrics_variant, 6);

  fprintf_event_sequences_from_tuple (output_stream, metrics_variant, 7);

  g_variant_unref (metrics_variant);
}

/**
 * Converts the given char to an unsigned char with a numeric value
 * guaranteed to be less than 16. The given char must be a hexadecimal
 * character. If it isn't, then the program exits. If the given char
 * is not a hexadecimal character, and the program hasn't exited,
 * then the function returns -1.
 */
static inline guchar
hexdigit_to_num (gchar digit)
{
  if (!isxdigit(digit))
  {
    fprintf(stderr, "The given hex string has an invalid hexadecimal character.\n");
    exit(1);
  }

  guchar retval = (digit >= '0' && digit <= '9')? digit - '0'
                : (digit >= 'A' && digit <= 'F')? digit - 'A' + 10
                : (digit >= 'a' && digit <= 'f')? digit - 'a' + 10
                : 0xFF; // Make sure that this never happens.
  g_assert (retval <= 0xF);
  return retval;
}

/**
 * Converts the given hexstring to bytes of data and sets them in data.
 * The length of the given hexstring must be equal to twice the number
 * of bytes in data.
 * Hexstring is upper or lower case hexadecimal, NOT prepended with "0x"
 * If the given hexstring is not a valid hexadecimal string, then the
 * program exits.
 */
void hex2data(guchar data[],
              const gchar *hexstring)
{
  while(*hexstring != '\0')
  {
    *data = hexdigit_to_num(*hexstring++) << 4;
    *data++ |= hexdigit_to_num(*hexstring++);
  }
}

int main(int argc,
         const char* argv[])
{
  FILE *output_stream = stdout;

  if (argc == 1 || argc > 3)
  {
    fprintf(stderr, "Usage: eos-metric-parser hexadecimalstring [FILE]\n");
    return EX_USAGE;
  }

  if (argc == 3)
  {
    const char *file_path = argv[2];
    FILE *output_file = fopen (file_path, "a");

    if (output_file == NULL)
    {
      fprintf(stderr, "Error: Could not open file - %s\n", file_path);
      return 1;
    }

    output_stream = output_file;
  }

  const gchar *hex_string = argv[1];
  size_t hex_string_length = strlen(hex_string);

  if (hex_string_length % 2 != 0)
  {
    fprintf(stderr, "The given hex string has an odd length.\n");
    return 1;
  }

  size_t raw_metric_request_length = hex_string_length / 2;
  guchar raw_metric_request[raw_metric_request_length];

  // Convert hex string to C data
  hex2data(raw_metric_request, hex_string);

  fprintf_serialized_metrics(output_stream, raw_metric_request, raw_metric_request_length);

  if (output_stream != stdout)
    fclose(output_stream);

  return 0;
}
