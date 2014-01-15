/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-connection.h"
#include "emtr-mac-private.h"
#include "emtr-util-private.h"
#include "emtr-uuid-private.h"
#include "emtr-web-private.h"

#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

/**
 * SECTION:emtr-connection
 * @title: Connection
 * @short_description: Connection to the metrics server
 * @include: eosmetrics/eosmetrics.h
 *
 * Represents a connection to a metrics collection server.
 * Usually you will not have to create this object yourself; the #EmtrSender
 * will create one with default values.
 */

#define DEFAULT_ENDPOINT "http://localhost:3000"
#define USERNAME "endlessos"
#define PASSWORD "sosseldne"

typedef struct
{
  gchar *uri_context;
  gchar *uri;
  gchar *form_param_name;
  gchar *endpoint;
  gchar *fingerprint;
  gint64 mac_address;

  GFile *endpoint_config_file;
  GFile *fingerprint_file;
} EmtrConnectionPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmtrConnection, emtr_connection, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_URI_CONTEXT,
  PROP_FORM_PARAM_NAME,
  PROP_ENDPOINT_CONFIG_FILE,
  PROP_FINGERPRINT_FILE,
  PROP_ENDPOINT,
  NPROPS
};

static GParamSpec *emtr_connection_props[NPROPS] = { NULL, };

static void
emtr_connection_constructed (GObject *object)
{
  EmtrConnection *self = EMTR_CONNECTION (object);
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);

  /* Set default values for GObject construct-only properties */
  if (priv->endpoint_config_file == NULL || priv->fingerprint_file == NULL)
    {
      GFile *eos_metrics_dir = emtr_get_data_dir ();

      if (priv->endpoint_config_file == NULL)
        priv->endpoint_config_file = g_file_get_child (eos_metrics_dir,
                                                       "endpoint.json");

      if (priv->fingerprint_file == NULL)
        priv->fingerprint_file = g_file_get_child (eos_metrics_dir,
                                                   "fingerprint");

      g_object_unref (eos_metrics_dir);
    }

  /* Set real values for mock-able functions (can be overridden from test code
  later) */
  self->_uuid_gen_func = emtr_uuid_gen;
  self->_mac_gen_func = emtr_mac_gen;
  self->_web_send_sync_func = emtr_web_post_authorized_sync;
  self->_web_send_async_func = emtr_web_post_authorized;
  self->_web_send_finish_func = emtr_web_post_authorized_finish;
}

static gchar *
get_endpoint_from_file (GFile *file)
{
  JsonParser *parser = json_parser_new ();
  gchar *filename = g_file_get_path (file);
  GError *error = NULL;

  if (!json_parser_load_from_file (parser, filename, &error))
    {
      g_debug ("Error loading endpoint file '%s': %s", filename, error->message);
      g_error_free (error);
      g_free (filename);
      return NULL;
    }
  g_free (filename);

  JsonReader *reader = json_reader_new (json_parser_get_root (parser));
  json_reader_read_member (reader, "endpoint");
  gchar *retval = g_strdup (json_reader_get_string_value (reader));
  json_reader_end_member (reader);
  g_object_unref (reader);
  g_object_unref (parser);

  if (retval == NULL)
    {
      g_warning ("Error loading endpoint file");
      return NULL;
    }
  return retval;
}

static const gchar *
get_uri (EmtrConnection *self)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);

  if (priv->uri == NULL)
    {
      priv->uri = g_strconcat (emtr_connection_get_endpoint (self), "/",
                               priv->uri_context, NULL);
    }
  return priv->uri;
}

static const gchar *
get_fingerprint (EmtrConnection *self)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  GError *error = NULL;

  if (priv->fingerprint == NULL)
    {
      if (!g_file_load_contents (priv->fingerprint_file, NULL,
                                 &priv->fingerprint, NULL, NULL, NULL))
        {
          /* ignore error, just create a new fingerprint file */
          priv->fingerprint = self->_uuid_gen_func ();

          /* ignore errors creating fingerprint directory */
          GFile *parent_dir = g_file_get_parent (priv->fingerprint_file);
          g_file_make_directory_with_parents (parent_dir, NULL, NULL);
          g_object_unref (parent_dir);

          if (!g_file_replace_contents (priv->fingerprint_file,
                                        priv->fingerprint,
                                        strlen (priv->fingerprint),
                                        NULL, FALSE,
                                        G_FILE_CREATE_REPLACE_DESTINATION,
                                        NULL, NULL, &error))
            {
              g_critical ("Error writing fingerprint file: %s", error->message);
              g_error_free (error);
            }
        }
    }
  g_assert (priv->fingerprint != NULL);
  return priv->fingerprint;
}

static gint64
get_mac_address (EmtrConnection *self)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);

  if (priv->mac_address == -1)
    {
      priv->mac_address = self->_mac_gen_func ();
    }
  return priv->mac_address;
}

static void
emtr_connection_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  EmtrConnection *self = EMTR_CONNECTION (object);

  switch (property_id)
    {
    case PROP_URI_CONTEXT:
      g_value_set_string (value, emtr_connection_get_uri_context (self));
      break;

    case PROP_FORM_PARAM_NAME:
      g_value_set_string (value, emtr_connection_get_form_param_name (self));
      break;

    case PROP_ENDPOINT_CONFIG_FILE:
      g_value_set_object (value,
                          emtr_connection_get_endpoint_config_file (self));
      break;

    case PROP_FINGERPRINT_FILE:
      g_value_set_object (value, emtr_connection_get_fingerprint_file (self));
      break;

    case PROP_ENDPOINT:
      g_value_set_string (value, emtr_connection_get_endpoint (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
set_uri_context (EmtrConnection *self,
                 const gchar    *uri_context)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  g_free (priv->uri_context);
  priv->uri_context = g_strdup (uri_context);
}

static void
set_form_param_name (EmtrConnection *self,
                     const gchar    *form_param_name)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  g_free (priv->form_param_name);
  priv->form_param_name = g_strdup (form_param_name);
}

static void
set_endpoint_config_file (EmtrConnection *self,
                          GFile          *file)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  if (priv->endpoint_config_file != NULL)
    g_object_unref (priv->endpoint_config_file);
  priv->endpoint_config_file = file;
  if (priv->endpoint_config_file != NULL)
    g_object_ref (priv->endpoint_config_file);
  g_clear_pointer (&priv->endpoint, g_free);
}

static void
set_fingerprint_file (EmtrConnection *self,
                      GFile          *file)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  if (priv->fingerprint_file != NULL)
    g_object_unref (priv->fingerprint_file);
  priv->fingerprint_file = file;
  if (priv->fingerprint_file != NULL)
    g_object_ref (priv->fingerprint_file);
}

static void
emtr_connection_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  EmtrConnection *self = EMTR_CONNECTION (object);

  switch (property_id)
    {
    case PROP_URI_CONTEXT:
      set_uri_context (self, g_value_get_string (value));
      break;

    case PROP_FORM_PARAM_NAME:
      set_form_param_name (self, g_value_get_string (value));
      break;

    case PROP_ENDPOINT_CONFIG_FILE:
      set_endpoint_config_file (self, g_value_get_object (value));
      break;

    case PROP_FINGERPRINT_FILE:
      set_fingerprint_file (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emtr_connection_finalize (GObject *object)
{
  EmtrConnection *self = EMTR_CONNECTION (object);
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);

  g_clear_pointer (&priv->uri_context, g_free);
  g_clear_pointer (&priv->uri, g_free);
  g_clear_pointer (&priv->form_param_name, g_free);
  g_clear_pointer (&priv->endpoint, g_free);
  g_clear_pointer (&priv->fingerprint, g_free);
  g_clear_object (&priv->endpoint_config_file);
  g_clear_object (&priv->fingerprint_file);

  G_OBJECT_CLASS (emtr_connection_parent_class)->finalize (object);
}

static void
emtr_connection_class_init (EmtrConnectionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = emtr_connection_constructed;
  object_class->get_property = emtr_connection_get_property;
  object_class->set_property = emtr_connection_set_property;
  object_class->finalize = emtr_connection_finalize;

  /**
   * EmtrConnection:uri-context:
   *
   * This is a URI component, relative to the root of the metrics server
   * endpoint, to which the metrics data is posted.
   * For example, if the #EmtrConnection:endpoint is
   * <uri>http://example.com</uri> and the #EmtrConnection:uri-context is set to
   * <code>"metrics"</code>, then the metrics data is posted to
   * <uri>http://example.com/metrics</uri>.
   */
  emtr_connection_props[PROP_URI_CONTEXT] =
    g_param_spec_string ("uri-context", "URI context",
                         "URI relative to the metrics server to post to",
                         "metrics",
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * EmtrConnection:form-param-name:
   *
   * This is a valid JavaScript property name under which the payload is
   * inserted into the HTTP POST message body when posting the data to the
   * metrics server.
   * For example, when sending the following payload with a
   * #EmtrConnection:form-param-name of <code>"data"</code>,
   * |[
   * {
   *     "clicks": 5,
   *     "timestamp": 1234
   * }
   * ]|
   * the resulting HTTP message body will look like this:
   * |[
   * {
   *     "data": {
   *         "clicks": 5,
   *         "timestamp": 1234
   *     }
   * }
   * ]|
   */
  emtr_connection_props[PROP_FORM_PARAM_NAME] =
    g_param_spec_string ("form-param-name", "Form param name",
                         "Property name under which to insert the payload into the HTTP POST data",
                         "data",
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * EmtrConnection:endpoint-config-file:
   *
   * A #GFile handle to a file containing the endpoint to use for metrics
   * collection.
   * The endpoint is the address of the metrics collection server.
   * The file should contain JSON data with the following format:
   * |[
   * {
   *    "endpoint": "http://example.com"
   * }
   * ]|
   *
   * If the file does not exist, the default address of
   * <uri>http://localhost:3000</uri> will be used.
   *
   * Setting this property to %NULL will use the default file location of
   * <filename><varname>$XDG_DATA_HOME</varname>/eosmetrics/endpoint.json</filename>.
   */
  emtr_connection_props[PROP_ENDPOINT_CONFIG_FILE] =
    g_param_spec_object ("endpoint-config-file", "Endpoint config file",
                         "Need description when I find out what this does FIXME",
                         G_TYPE_FILE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * EmtrConnection:fingerprint-file:
   *
   * A #GFile handle to a file containing this installation's fingerprint.
   *
   * If the file does not exist, a new fingerprint will be created and written
   * to the file.
   * The fingerprint should be unique to the operating system installation.
   *
   * Setting this property to %NULL will use the default file location of
   * <filename><varname>$XDG_DATA_HOME</varname>/eosmetrics/fingerprint</filename>.
   */
  emtr_connection_props[PROP_FINGERPRINT_FILE] =
    g_param_spec_object ("fingerprint-file", "Fingerprint file",
                         "File containing this machine's fingerprint",
                         G_TYPE_FILE,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  /**
   * EmtrConnection:endpoint:
   *
   * Address for the metrics collection server.
   * This is a read-only property; it can only be set at construct time by
   * providing a different value for #EmtrConnection:endpoint-config-file.
   */
  emtr_connection_props[PROP_ENDPOINT] =
    g_param_spec_string ("endpoint", "Endpoint URI",
                         "Address for the metrics collection server",
                         "",
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emtr_connection_props);
}

static void
emtr_connection_init (EmtrConnection *self)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  priv->mac_address = -1;
}

/* Turn the GVariant payload into JSON data in the form of a string. Free the
string with g_free when done. */
static gchar *
prepare_post_data (EmtrConnection *self,
                   GVariant       *payload)
{
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);

  JsonNode *payload_json = json_gvariant_serialize (payload);
  JsonObject *data_object = json_node_get_object (payload_json);
  json_object_set_string_member (data_object,
                                 "fingerprint", get_fingerprint (self));
  json_object_set_int_member (data_object,
                             "machine", get_mac_address (self));

  JsonObject *post_data_object = json_object_new ();
  json_object_set_object_member (post_data_object,
                                 priv->form_param_name, data_object);

  JsonNode *post_data_node = json_node_alloc ();
  json_node_init_object (post_data_node, post_data_object);

  JsonGenerator *stringify = json_generator_new ();
  json_generator_set_root (stringify, post_data_node);
  gchar *post_data = json_generator_to_data (stringify, NULL);

  json_node_free (payload_json);
  json_node_free (post_data_node);
  g_object_unref (stringify);

  return post_data;
}

/* Moves (does not copy) inner_error into error */
static void
interpret_send_error (EmtrConnection *self,
                      GError         *inner_error,
                      GError        **error)
{
  g_propagate_prefixed_error (error, inner_error,
                              "Error sending metrics data to %s@%s: ",
                              USERNAME, get_uri (self));
}

/* Callback for the async operation of emtr_connection_send() */
static void
send_async_callback (GObject      *unused,
                     GAsyncResult *result,
                     GTask        *task)
{
  GError *inner_error = NULL;
  GError *error = NULL;
  EmtrConnection *self = EMTR_CONNECTION (g_task_get_source_object (task));

  gboolean success = self->_web_send_finish_func (result, &inner_error);
  if (success)
    {
      g_task_return_boolean (task, TRUE);
    }
  else
    {
      interpret_send_error (self, inner_error, &error);
      g_task_return_error (task, error);
    }
}

/* PUBLIC API */

/**
 * emtr_connection_new:
 * @uri_context: Initial value for EmtrConnection:uri-context
 * @form_param_name: Initial value for EmtrConnection:form-param-name
 * @endpoint_config_file: (allow-none): Initial value for
 * EmtrConnection:endpoint-config-file
 * @fingerprint_file: (allow-none): Initial value for
 * EmtrConnection:fingerprint-file
 *
 * Convenience function in the C API for creating a new #EmtrConnection.
 * It is only useful if you want to specify all four parameters.
 *
 * Returns: a new #EmtrConnection. Free with g_object_unref() when done.
 */
EmtrConnection *
emtr_connection_new (const gchar *uri_context,
                     const gchar *form_param_name,
                     GFile       *endpoint_config_file,
                     GFile       *fingerprint_file)
{
  g_return_val_if_fail (uri_context != NULL, NULL);
  g_return_val_if_fail (form_param_name != NULL, NULL);
  g_return_val_if_fail (endpoint_config_file == NULL
                        || G_IS_FILE (endpoint_config_file),
                        NULL);
  g_return_val_if_fail (fingerprint_file == NULL
                        || G_IS_FILE (fingerprint_file),
                        NULL);
  return g_object_new (EMTR_TYPE_CONNECTION,
                       "uri-context", uri_context,
                       "form-param-name", form_param_name,
                       "endpoint-config-file", endpoint_config_file,
                       "fingerprint-file", fingerprint_file,
                       NULL);
}

/**
 * emtr_connection_get_uri_context:
 * @self: the metrics connection
 *
 * See #EmtrConnection:uri-context.
 *
 * Returns: the URI context as a string
 */
const gchar *
emtr_connection_get_uri_context (EmtrConnection *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_CONNECTION (self), NULL);
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  return priv->uri_context;
}

/**
 * emtr_connection_get_form_param_name:
 * @self: the metrics connection
 *
 * See #EmtrConnection:form-param-name.
 *
 * Returns: the form param name as a string
 */
const gchar *
emtr_connection_get_form_param_name (EmtrConnection *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_CONNECTION (self), NULL);
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  return priv->form_param_name;
}

/**
 * emtr_connection_get_endpoint_config_file:
 * @self: the metrics connection
 *
 * See #EmtrConnection:endpoint-config-file.
 *
 * Returns: (transfer none): a handle to the endpoint config file
 */
GFile *
emtr_connection_get_endpoint_config_file (EmtrConnection *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_CONNECTION (self), NULL);
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  return priv->endpoint_config_file;
}

/**
 * emtr_connection_get_fingerprint_file:
 * @self: the metrics connection
 *
 * See #EmtrConnection:fingerprint-file.
 *
 * Returns: (transfer none): a handle to the fingerprint file
 */
GFile *
emtr_connection_get_fingerprint_file (EmtrConnection *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_CONNECTION (self), NULL);
  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);
  return priv->fingerprint_file;
}

/**
 * emtr_connection_get_endpoint:
 * @self: the metrics connection
 *
 * See #EmtrConnection:endpoint.
 *
 * Returns: the endpoint as a string
 */
const gchar *
emtr_connection_get_endpoint (EmtrConnection *self)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_CONNECTION (self), NULL);

  EmtrConnectionPrivate *priv = emtr_connection_get_instance_private (self);

  if (priv->endpoint == NULL)
    {
      priv->endpoint = get_endpoint_from_file (priv->endpoint_config_file);
      if (priv->endpoint == NULL)
        priv->endpoint = g_strdup (DEFAULT_ENDPOINT);
      g_debug ("Using endpoint %s for metrics collection", priv->endpoint);
    }

  return priv->endpoint;
}

/**
 * emtr_connection_send_sync:
 * @self: the metrics connection
 * @payload: a #GVariant with the data to send
 * @cancellable: (allow-none): currently unused, pass %NULL
 * @error: return location for an error, or %NULL
 *
 * Posts the metrics data specified by @payload to the metrics server referenced
 * by the endpoint of this connection (see #EmtrConnection:endpoint,
 * #EmtrConnection:endpoint-config-file.)
 * The data @payload must be in the form of a #GVariant that has the
 * <code>a{sv}</code> type; it is converted into JSON for sending.
 *
 * <note><para>
 *   This a synchronous version of emtr_connection_send().
 *   It may block if the operation takes a long time.
 *   Use emtr_connection_send() unless you know what you're doing.
 * </para></note>
 *
 * Returns: %TRUE on success, or %FALSE on failure.
 */
gboolean
emtr_connection_send_sync (EmtrConnection *self,
                           GVariant       *payload,
                           GCancellable   *cancellable,
                           GError        **error)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_CONNECTION (self), FALSE);
  g_return_val_if_fail (payload != NULL
                        && g_variant_is_of_type (payload, G_VARIANT_TYPE_VARDICT),
                        FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  gchar *post_data = prepare_post_data (self, payload);

  GError *inner_error = NULL;
  gboolean success = self->_web_send_sync_func (get_uri (self), post_data,
                                                USERNAME, PASSWORD, cancellable,
                                                &inner_error);
  if (!success)
    interpret_send_error (self, inner_error, error);

  g_free (post_data);
  return success;
}

/**
 * emtr_connection_send:
 * @self: the metrics connection
 * @payload: a #GVariant with the data to send
 * @cancellable: (allow-none): currently unused, pass %NULL
 * @callback: (scope async): function to call when the operation is finished
 * @user_data: (closure): extra parameter to pass to @callback
 *
 * Starts asynchronously posting the metrics data specified by @payload to the
 * metrics server referenced by the endpoint of this connection (see
 * #EmtrConnection:endpoint, #EmtrConnection:endpoint-config-file.)
 * The data @payload must be in the form of a #GVariant that has the
 * <code>a{sv}</code> type; it is converted into JSON for sending.
 *
 * When the operation has completed, @callback will be called and @user_data
 * will be passed to it.
 * Inside @callback, you must finalize the operation with
 * emtr_connection_send_finish().
 */
void
emtr_connection_send (EmtrConnection     *self,
                      GVariant           *payload,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
  g_return_if_fail (self != NULL && EMTR_IS_CONNECTION (self));
  g_return_if_fail (payload != NULL
                    && g_variant_is_of_type (payload, G_VARIANT_TYPE_VARDICT));
  g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (callback != NULL);

  gchar *post_data = prepare_post_data (self, payload);
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, post_data, g_free);
  self->_web_send_async_func (get_uri (self), post_data, USERNAME, PASSWORD,
                              cancellable,
                              (GAsyncReadyCallback)send_async_callback, task);
}

/**
 * emtr_connection_send_finish:
 * @self: the metrics connection
 * @result: a #GAsyncResult
 * @error: return location for a #GError, or %NULL
 *
 * Finishes an operation begun by emtr_connection_send().
 *
 * Returns: %TRUE if everything succeeded, or %FALSE if not, in which case
 * @error is set.
 */
gboolean
emtr_connection_send_finish (EmtrConnection *self,
                             GAsyncResult   *result,
                             GError        **error)
{
  g_return_val_if_fail (self != NULL && EMTR_IS_CONNECTION (self), FALSE);
  g_return_val_if_fail (result != NULL && g_task_is_valid (result, self),
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}
