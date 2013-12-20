/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-sender.h"
#include "emtr-connection.h"
#include "emtr-util.h"

#include <string.h>
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

 #define EMPTY_QUEUE "[]"

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
get_data_from_file (GFile        *file,
                    GCancellable *cancellable,
                    GError      **error)
{
  GFileInputStream *stream = g_file_read (file, cancellable, error);
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
                                     cancellable, error))
    {
      g_object_unref (parser);
      g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL); /*ignore err*/
      if ((*error)->domain == JSON_PARSER_ERROR) {
        /* File contained invalid JSON data; return an empty array, but print a
        warning message */
        g_warning ("Storage file contained invalid JSON data, ignoring.");
        g_clear_error (error);
        return create_empty_json_array ();
      }
      return NULL;
    }
  if (!g_input_stream_close (G_INPUT_STREAM (stream), cancellable, error))
    {
      g_object_unref (parser);
      return NULL;
    }

  JsonNode *retval = json_node_copy (json_parser_get_root (parser));
  g_object_unref (parser);
  return retval;
}

/* Helper function: save JSON data to a file. */
static gboolean
save_data_to_file (GFile        *file,
                   JsonNode     *json_data,
                   GCancellable *cancellable,
                   GError      **error)
{
  GFileOutputStream *stream = g_file_replace (file, NULL /* etag */,
                                              FALSE, /* make backup */
                                              G_FILE_CREATE_REPLACE_DESTINATION,
                                              cancellable, error);
  if (stream == NULL)
    return FALSE;

  JsonGenerator *generator = json_generator_new ();
  json_generator_set_root (generator, json_data);
  gboolean success = json_generator_to_stream (generator,
                                               G_OUTPUT_STREAM (stream),
                                               cancellable, error);
  success |= g_output_stream_close (G_OUTPUT_STREAM (stream), cancellable,
                                    error);
  return success;
}

/* Helper function: save the data payload to a queueing file. Return FALSE and
set error if the data was not saved. */
static gboolean
save_payload (EmtrSender   *self,
              GVariant     *payload,
              GCancellable *cancellable,
              GError      **error)
{
  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  GError *inner_error = NULL;

  JsonNode *queued_data = get_data_from_file (priv->storage_file,
                                              cancellable, &inner_error);
  if (queued_data == NULL)
    {
      g_propagate_prefixed_error (error, inner_error,
                                  "Error reading queued file: ");
      return FALSE;
    }
  JsonArray *data_list = json_node_get_array (queued_data);
  JsonNode *new_node = json_gvariant_serialize (payload);
  json_array_add_element (data_list, new_node);

  if (!save_data_to_file (priv->storage_file, queued_data,
                          cancellable, &inner_error))
    {
      json_node_free (queued_data);
      g_propagate_prefixed_error (error, inner_error,
                                  "Error saving payload to queue: ");
      return FALSE;
    }
  json_node_free (queued_data);

  return TRUE;
}

static void
interpret_save_payload_error (GError  *inner_error,
                              GError **error)
{
  g_propagate_prefixed_error (error, inner_error,
                              "Metrics data could neither be sent nor queued: ");
}

static void
interpret_send_error (GError **error)
{
  g_debug ("Queueing metrics data because sending failed: %s",
           (*error)->message);
  g_clear_error (error);
}

/* This could be improved by making the save_payload(), get_data_from_file(),
and save_data_to_file() operations asynchronous as well; but for now they are
just cancellable, since it's not likely they'll take up very much time. */
static void
send_async_callback (EmtrConnection *connection,
                     GAsyncResult   *result,
                     GTask          *task)
{
  GError *error = NULL;
  GError *inner_error = NULL;
  gboolean success = emtr_connection_send_finish (connection, result, &error);
  if (success) {
    g_task_return_boolean (task, TRUE);
    return;
  }
  interpret_send_error (&error);

  EmtrSender *self = EMTR_SENDER (g_task_get_source_object (task));
  GVariant *payload = g_task_get_task_data (task);
  if (save_payload (self, payload, g_task_get_cancellable (task), &inner_error))
    {
      g_task_return_boolean (task, TRUE);
      return;
    }
  interpret_save_payload_error (inner_error, &error);
  g_task_return_error (task, error);
}

struct SendQueuedData {
  gboolean success;
  GCancellable *cancellable;
  GError **error;  /* struct does not own it */
  EmtrSender *self;
};

static void
foreach_payload_in_queue_sync (JsonArray             *old_queue,
                               guint                  ix,
                               JsonNode              *element_node,
                               struct SendQueuedData *data)
{
  if (!data->success)
    return;
  /* NB. setting data->success = FALSE and returning is equivalent to
  "break"-ing out of the foreach loop, of which this function is the loop body;
  since there's no way to interrupt the loop with a boolean return value */

  GError *error = NULL;
  GVariant *payload = json_gvariant_deserialize (element_node, "a{sv}", &error);
  if (payload == NULL)
    {
      data->success = FALSE;
      g_propagate_prefixed_error (data->error, error,
                                  "Error converting JSON, data may have been dropped: ");
      return;  /* i.e. "break" */
    }
  data->success = emtr_sender_send_data_sync (data->self, payload,
                                              data->cancellable, &error);
  g_variant_unref (payload);
  if (!data->success)
    g_propagate_prefixed_error (data->error, error, "Data was dropped: ");
}

static void
send_queued_data_sync_thread (GTask        *task,
                              EmtrSender   *self,
                              gpointer      unused,
                              GCancellable *cancellable)
{
  GError *error = NULL;
  if (!emtr_sender_send_queued_data_sync (self, cancellable, &error))
    {
      g_task_return_error (task, error);
      return;
    }
  g_task_return_boolean (task, TRUE);
  return;
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
 * emtr_sender_send_data_sync:
 * @self: the send process
 * @payload: a #GVariant with the data to send
 * @cancellable: (allow-none): a #GCancellable, or %NULL to ignore
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
 * To cancel the operation, pass a non-%NULL #GCancellable object to
 * @cancellable and trigger it from another thread.
 * If this operation is cancelled, then emtr_sender_send_data_finish() will
 * return %FALSE with the error %G_IO_ERROR_CANCELLED.
 * Note that currently the queueing part of the operation is the only part that
 * can be cancelled; interrupting the transmission to the metrics server is
 * not supported yet.
 *
 * Note that the return value from this function does not tell you whether
 * @payload was actually <emphasis>sent</emphasis> to the server.
 * A return value of %TRUE means the data was processed: either sent, or queued
 * to be sent later; your application can safely forget about the data.
 *
 * <note><para>
 *   This a synchronous version of emtr_sender_send_data().
 *   It may block if the operation takes a long time.
 *   Use emtr_sender_send_data() unless you know what you're doing.
 * </para></note>
 *
 * Returns: %TRUE if the data was processed, %FALSE otherwise, in which case
 * @error is set.
 */
gboolean
emtr_sender_send_data_sync (EmtrSender   *self,
                            GVariant     *payload,
                            GCancellable *cancellable,
                            GError      **error)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_SENDER (self), FALSE);
  g_return_val_if_fail (payload != NULL
                        && g_variant_is_of_type (payload, G_VARIANT_TYPE_VARDICT),
                        FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  GError *inner_error = NULL;
  if (emtr_connection_send_sync (priv->connection, payload,
                                 cancellable, &inner_error))
    return TRUE;
  interpret_send_error (&inner_error);
  if (save_payload (self, payload, cancellable, &inner_error))
    return TRUE;
  interpret_save_payload_error (inner_error, error);
  return FALSE;
}

/**
 * emtr_sender_send_data:
 * @self: the send process
 * @payload: a #GVariant with the data to send
 * @cancellable: (allow-none): a #GCancellable, or %NULL to ignore
 * @callback: (scope async): function to call when the operation is finished
 * @user_data: (closure): extra parameter to pass to @callback
 *
 * Starts asynchronously posting the metrics data specified by @payload to a
 * metrics server (see #EmtrSender:connection for how to specify which one.)
 * The data @payload must be in the form of a #GVariant that has the
 * <code>a{sv}</code> type; it is converted into JSON for sending.
 *
 * To cancel the operation, pass a non-%NULL #GCancellable object to
 * @cancellable and trigger it from another thread.
 * If this operation is cancelled, then emtr_sender_send_data_finish() will
 * return %FALSE with the error %G_IO_ERROR_CANCELLED.
 * Note that currently the queueing part of the operation is the only part that
 * can be cancelled; interrupting the transmission to the metrics server is
 * not supported yet.
 *
 * If the sending fails, the data will be queued in the storage file (see
 * #EmtrSender:storage-file for how to specify where the storage file
 * lives.)
 * Queued data will be sent later.
 *
 * When the operation has completed, @callback will be called and @user_data
 * will be passed to it.
 * Inside @callback, you must finalize the operation with
 * emtr_sender_send_data_finish().
 */
void
emtr_sender_send_data (EmtrSender         *self,
                       GVariant           *payload,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data)
{
  g_return_if_fail (self != NULL && EMTR_IS_SENDER (self));
  g_return_if_fail (payload != NULL
                    && g_variant_is_of_type (payload, G_VARIANT_TYPE_VARDICT));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (callback != NULL);

  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, payload, NULL);
  emtr_connection_send (priv->connection, payload, cancellable,
                        (GAsyncReadyCallback)send_async_callback, task);
}

/**
 * emtr_sender_send_data_finish:
 * @self: the send process
 * @result: a #GAsyncResult
 * @error: return location for a #GError, or %NULL
 *
 * Finishes an operation begun by emtr_sender_send_data().
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
emtr_sender_send_data_finish (EmtrSender   *self,
                              GAsyncResult *result,
                              GError      **error)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_SENDER (self), FALSE);
  g_return_val_if_fail (result != NULL && g_task_is_valid (result, self),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * emtr_sender_send_queued_data:
 * @self: the send process
 * @cancellable: (allow-none): a #GCancellable, or %NULL to ignore
 * @callback: (scope async): function to call when the operation is finished
 * @user_data: (closure): extra parameter to pass to @callback
 *
 * Attempts to post the metrics data stored in this sender's queue (if there is
 * any) to a metrics server (see #EmtrSender:connection for how to specify which
 * one.)
 *
 * To cancel the operation, pass a non-%NULL #GCancellable object to
 * @cancellable and trigger it from another thread.
 * If this operation is cancelled, then emtr_sender_send_queued_data_finish()
 * will return %FALSE with the error %G_IO_ERROR_CANCELLED.
 *
 * Note that you cannot get information about what is in the queue.
 * In fact, all the data may still be in the queue when the operation is done,
 * if it still couldn't be sent.
 *
 * When the operation has completed, @callback will be called and @user_data
 * will be passed to it.
 * Inside @callback, you must finalize the operation with
 * emtr_sender_send_data_finish().
 *
 * An example use of this function would be at the beginning and end of an
 * application.
 */
void
emtr_sender_send_queued_data (EmtrSender         *self,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
  g_return_if_fail (self != NULL && EMTR_IS_SENDER (self));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (callback != NULL);

  GTask *task = g_task_new (self, cancellable, callback, user_data);

  /* We do the sync operation in a worker thread here, since it consists of many
  blocking operations and a clean threaded implementation is less likely to be
  buggy than a callback hell implementation */
  g_task_run_in_thread (task, (GTaskThreadFunc)send_queued_data_sync_thread);
}

/**
 * emtr_sender_send_queued_data_finish:
 * @self: the send process
 * @result: a #GAsyncResult
 * @error: return location for a #GError, or %NULL
 *
 * Finishes an operation begun by emtr_sender_send_queued_data().
 *
 * Note that the return value from this function does not tell you whether
 * emptying the queue was successful.
 * The data may still be in the queue if it couldn't be sent.
 *
 * Returns: %TRUE if the operation was successful, %FALSE otherwise, in which
 * case @error is set.
 */
gboolean
emtr_sender_send_queued_data_finish (EmtrSender   *self,
                                     GAsyncResult *result,
                                     GError      **error)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_SENDER (self), FALSE);
  g_return_val_if_fail (result != NULL && g_task_is_valid (result, self),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * emtr_sender_send_queued_data_sync:
 * @self: the send process
 * @cancellable: (allow-none): a #GCancellable, or %NULL to ignore
 * @error: return location for an error, or %NULL
 *
 * Attempts to post the metrics data stored in this sender's queue (if there is
 * any) to a metrics server (see #EmtrSender:connection for how to specify which
 * one.)
 * This function waits until the attempt is finished.
 *
 * To cancel the operation, pass a non-%NULL #GCancellable object to
 * @cancellable and trigger it from another thread.
 * If this operation is cancelled, then this will return %FALSE with the error
 * %G_IO_ERROR_CANCELLED.
 * Note that cancelling may cause you to lose unsent metrics data, or to have
 * metrics data still in the queue that has also been sent to the server.
 *
 * Note that you cannot get information about what is in the queue.
 * In fact, all the data may still be in the queue when the operation is done,
 * if it still couldn't be sent.
 *
 * <note><para>
 *   This a synchronous version of emtr_sender_send_queued_data().
 *   It may block if the operation takes a long time.
 *   Use emtr_sender_send_queued_data() unless you know what you're doing.
 * </para></note>
 *
 * Returns: %TRUE if the data was processed, %FALSE otherwise, in which case
 * @error is set.
 */
gboolean
emtr_sender_send_queued_data_sync (EmtrSender   *self,
                                   GCancellable *cancellable,
                                   GError      **error)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_SENDER (self), FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  EmtrSenderPrivate *priv = emtr_sender_get_instance_private (self);
  GError *inner_error = NULL;

  JsonNode *old_queue_node = get_data_from_file (priv->storage_file,
                                                 cancellable, &inner_error);
  if (old_queue_node == NULL)
    {
      g_propagate_prefixed_error (error, inner_error,
                                  "Error reading queued file: ");
      return FALSE;
    }
  JsonArray *old_queue = json_node_get_array (old_queue_node);

  if (!g_file_replace_contents (priv->storage_file, EMPTY_QUEUE,
                                strlen (EMPTY_QUEUE), NULL /* etag */,
                                FALSE /* backup */,
                                G_FILE_CREATE_REPLACE_DESTINATION,
                                NULL /* new etag */, cancellable, &inner_error))
    {
      g_propagate_prefixed_error (error, inner_error, "Error clearing queue: ");
      return FALSE;
    }

  struct SendQueuedData data = {
    .success = TRUE,
    .cancellable = cancellable,
    .error = error,
    .self = self
  };

  json_array_foreach_element (old_queue,
                              (JsonArrayForeach)foreach_payload_in_queue_sync,
                              &data);

  json_node_free (old_queue_node);
  return data.success;
}
