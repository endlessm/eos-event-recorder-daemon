/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-sender.h"
#include "emtr-connection.h"
#include "emtr-util.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

/**
 * SECTION:emtr-sender
 * @title: Sender
 * @short_description: Handles sending data or queueing it to be sent later
 * @include: eosmetrics/eosmetrics.h
 *
 * The sender handles how and when metrics data gets sent to the metrics server.
 * It either sends it immediately if possible, or queues it to be sent later.
 * Once you give your data to emtr_sender_send_data(), you don't need to worry
 * about it anymore; the sender assumes the responsibility for making sure it
 * gets to its destination.
 */

typedef struct
{
  GFile *storage_file;
  EmtrConnection *connection;
} EmtrSenderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmtrSender, emtr_sender, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_STORAGE_FILE,
  PROP_CONNECTION,
  NPROPS
};

static GParamSpec *emtr_sender_props[NPROPS] = { NULL, };

static void
emtr_sender_get_property (GObject    *object,
                          guint       property_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  EmtrSender *self = EMTR_SENDER (object);
  switch (property_id)
    {
    case PROP_STORAGE_FILE:
      g_value_set_object (value, emtr_sender_get_storage_file (self));
      break;

    case PROP_CONNECTION:
      g_value_set_object (value, emtr_sender_get_connection (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

/* Helper function: the output is a GFile with an absolute path that this code
holds exactly one reference to (given that this code held zero references to the
passed-in file.) If it is a relative path, interpret it as being relative to the
default metrics storage directory. */
static GFile *
ensure_absolute_path_and_reference_or_null (GFile *file)
{
  if (file == NULL)
    return NULL;

  GFile *root = g_file_new_for_path ("/");
  if (g_file_has_prefix (file, root))
    {
      g_object_ref (file);
      g_object_unref (root);
      return file;
    }

  GFile *default_storage_dir = emtr_get_default_storage_dir ();
  gchar *default_storage_path = g_file_get_path (default_storage_dir);
  g_object_unref (default_storage_dir);
  GFile *absolute = g_file_resolve_relative_path (file, default_storage_path);
  g_free(default_storage_path);
  return absolute;
}

static void
set_storage_file (EmtrSender *self,
                  GFile      *file)
{
  file = ensure_absolute_path_and_reference_or_null (file);

  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  if (priv->storage_file != NULL)
    g_object_unref (priv->storage_file);
  priv->storage_file = file;
}


static void
emtr_sender_set_property (GObject      *object,
                          guint         property_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  EmtrSender *self = EMTR_SENDER (object);

  switch (property_id)
    {
    case PROP_STORAGE_FILE:
      set_storage_file (self, g_value_get_object (value));
      break;

    case PROP_CONNECTION:
      emtr_sender_set_connection (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emtr_sender_finalize (GObject *object)
{
  EmtrSender *self = EMTR_SENDER (object);
  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);

  g_clear_object (&priv->storage_file);
  g_clear_object (&priv->connection);

  G_OBJECT_CLASS (emtr_sender_parent_class)->finalize (object);
}

static void
emtr_sender_class_init (EmtrSenderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = emtr_sender_get_property;
  object_class->set_property = emtr_sender_set_property;
  object_class->finalize = emtr_sender_finalize;

  /**
   * EmtrSender:storage-file:
   *
   * The file where the data is to be stored temporarily if it can't be sent
   * immediately.
   * If the #GFile is a handle to a relative path, it is considered to be
   * relative to the default directory for storing metrics (see
   * emtr_get_default_storage_dir().)
   */
  emtr_sender_props[PROP_STORAGE_FILE] =
    g_param_spec_object ("storage-file", "Storage file",
                         "File where unsent data is stored until it can be sent",
                         G_TYPE_FILE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * EmtrSender:connection:
   *
   * The #EmtrConnection representing the metrics server that this sender should
   * post to.
   */
  emtr_sender_props[PROP_CONNECTION] =
    g_param_spec_object ("connection", "Connection",
                         "Connection object for metrics server",
                         EMTR_TYPE_CONNECTION,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emtr_sender_props);
}

static void
emtr_sender_init (EmtrSender *self)
{
}

/* Helper function: create a JsonNode representing the empty array "[]" */
static JsonNode *
create_empty_json_array (void)
{
  JsonNode *retval = json_node_alloc ();
  JsonArray *array = json_array_new ();
  json_node_init_array (retval, array);
  json_array_unref (array);
  return retval;
}

/* Helper function: load JSON data from a file. If the file does not exist, or
the file does not contain valid JSON data, return an empty JSON array. */
static JsonNode *
get_data_from_file (GFile   *file,
                    GError **error)
{
  GFileInputStream *stream = g_file_read (file, NULL, error);
  if (stream == NULL) {
    if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      {
        /* File did not exist; silently return an empty array */
        g_clear_error (error);
        return create_empty_json_array ();
      }
    return NULL;
  }

  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_stream (parser, G_INPUT_STREAM (stream),
                                     NULL, error))
    {
      g_object_unref (parser);
      g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);
      if ((*error)->domain == JSON_PARSER_ERROR) {
        /* File contained invalid JSON data; return an empty array, but print a
        warning message */
        g_warning ("Storage file contained invalid JSON data, ignoring.");
        g_clear_error (error);
        return create_empty_json_array ();
      }
      return NULL;
    }
  g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL); /* ignore error */

  JsonNode *retval = json_node_copy (json_parser_get_root (parser));
  g_object_unref (parser);
  return retval;
}

/* Helper function: save JSON data to a file. */
static gboolean
save_data_to_file (GFile    *file,
                   JsonNode *json_data,
                   GError  **error)
{
  GFileOutputStream *stream = g_file_replace (file, NULL /* etag */,
                                              FALSE, /* make backup */
                                              G_FILE_CREATE_REPLACE_DESTINATION,
                                              NULL /* cancellable */, error);
  if (stream == NULL)
    return FALSE;

  JsonGenerator *generator = json_generator_new ();
  json_generator_set_root (generator, json_data);
  gboolean success = json_generator_to_stream (generator,
                                               G_OUTPUT_STREAM (stream), NULL,
                                               error);
  g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, NULL); /*ignore error*/

  return success;
}

/* Helper function: save the data payload to a queueing file. Return FALSE and
set error if the data was not saved. */
static gboolean
save_payload (EmtrSender *self,
              GVariant   *payload,
              GError    **error)
{
  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  GError *inner_error = NULL;

  JsonNode *queued_data = get_data_from_file (priv->storage_file, &inner_error);
  if (queued_data == NULL)
    {
      g_propagate_prefixed_error (error, inner_error,
                                  "Error reading queued file: ");
      return FALSE;
    }
  JsonArray *data_list = json_node_get_array (queued_data);
  JsonNode *new_node = json_gvariant_serialize (payload);
  json_array_add_element (data_list, new_node);

  if (!save_data_to_file (priv->storage_file, queued_data, &inner_error))
    {
      json_node_free (queued_data);
      g_propagate_prefixed_error (error, inner_error,
                                  "Error saving payload to queue: ");
      return FALSE;
    }
  json_node_free (queued_data);

  return TRUE;
}

/* PUBLIC API */

/**
 * emtr_sender_new:
 * @storage_file: a #GFile handle (see #EmtrSender:storage-file)
 *
 * Convenience function for creating a new #EmtrSender in the C API, while
 * setting all construct-only properties.
 *
 * Returns: (transfer full): a new #EmtrSender.
 * Free with g_object_unref() when done.
 */
EmtrSender *
emtr_sender_new (GFile *storage_file)
{
  g_return_val_if_fail (storage_file != NULL && G_IS_FILE (storage_file), NULL);
  return g_object_new (EMTR_TYPE_SENDER,
                       "storage-file", storage_file,
                       NULL);
}

/**
 * emtr_sender_get_storage_file:
 * @self: the send process
 *
 * See #EmtrSender:storage-file.
 *
 * Returns: (transfer none): a #GFile handle to the storage file.
 * This object is owned by the #EmtrSender, do not unreference it.
 */
GFile *
emtr_sender_get_storage_file (EmtrSender *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_SENDER (self), NULL);
  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  return priv->storage_file;
}

/**
 * emtr_sender_get_connection:
 * @self: the send process
 *
 * See #EmtrSender:connection.
 *
 * Returns: (transfer none): an #EmtrConnection object.
 * This object is owned by the #EmtrSender, do not unreference it.
 */
EmtrConnection *
emtr_sender_get_connection (EmtrSender *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_SENDER (self), NULL);
  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  return priv->connection;
}

/**
 * emtr_sender_set_connection:
 * @self: the send process
 * @connection: an #EmtrConnection object
 *
 * See #EmtrSender:connection.
 */
void
emtr_sender_set_connection (EmtrSender *self,
                                  EmtrConnection  *connection)
{
  g_return_if_fail (self != NULL && EMTR_IS_SENDER (self));
  g_return_if_fail (connection == NULL || EMTR_IS_CONNECTION (connection));
  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);

  if (connection == NULL)
    connection = g_object_new (EMTR_TYPE_CONNECTION, NULL); /* all defaults */
  else
    g_object_ref (connection);

  if (priv->connection)
    g_object_unref (priv->connection);
  priv->connection = connection;
}

/**
 * emtr_sender_send_data:
 * @self: the send process
 * @payload: a #GVariant with the data to send
 * @error: return location for an error, or %NULL
 *
 * Posts the metrics data specified by @payload to a metrics server (see
 * #EmtrSender:connection for how to specify which one.)
 * The data @payload must be in the form of a #GVariant that has the
 * <code>a{sv}</code> type; it is converted into JSON for sending.
 *
 * If the sending fails, the data will be queued in the storage file (see
 * #EmtrSender:storage-file for how to specify where the storage file
 * lives.)
 * Queued data will be sent later.
 *
 * Note that the return value from this function does not tell you whether
 * @payload was actually <emphasis>sent</emphasis> to the server.
 * A return value of %TRUE means the data was processed: either sent, or queued
 * to be sent later; your application can safely forget about the data.
 *
 * Returns: %TRUE if the data was processed, %FALSE otherwise, in which case
 * @error is set.
 */
gboolean
emtr_sender_send_data (EmtrSender *self,
                       GVariant   *payload,
                       GError    **error)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_SENDER (self), FALSE);
  g_return_val_if_fail (payload != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  GError *inner_error = NULL;
  if (emtr_connection_send (priv->connection, payload, &inner_error))
    return TRUE;

  g_debug ("Queueing metrics data because sending failed: %s",
           inner_error->message);
  g_clear_error (&inner_error);

  if (save_payload (self, payload, &inner_error))
    return TRUE;
  g_propagate_prefixed_error (error, inner_error,
                              "Metrics data could neither be sent nor queued: ");
  return FALSE;
}
