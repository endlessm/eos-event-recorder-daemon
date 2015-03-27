/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014, 2015 Endless Mobile, Inc. */

/* This file is part of eos-event-recorder-daemon.
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

#include "emer-network-send-provider.h"

typedef struct EmerNetworkSendProviderPrivate
{
  gchar *path;
  gint send_number;
  gboolean data_cached;
  GKeyFile *key_file;
} EmerNetworkSendProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerNetworkSendProvider, emer_network_send_provider, G_TYPE_OBJECT)

/*
 * The filepath to the metadata file containing the network send metadata.
 */
#define DEFAULT_NETWORK_SEND_FILE_PATH PERSISTENT_CACHE_DIR "network_send_file"

#define NETWORK_SEND_GROUP "network_send_data"
#define NETWORK_SEND_KEY   "network_requests_sent"

enum
{
  PROP_0,
  PROP_PATH,
  NPROPS
};

static GParamSpec *emer_network_send_provider_props[NPROPS] = { NULL, };

/*
 * SECTION:emer-network-send-provider
 * @title: Network Send Provider
 * @short_description: Provides data regarding attempts to send metrics over the
 * network.
 *
 * The network send provider provides information on our attempts to send
 * network requests containing bundles of metrics to remote servers.
 * Specifically, it provides a "send_number" which indicates which attempt we
 * are making to send network requests. This value should be incremented every
 * time we package together metrics into a network request whether or not we
 * know if we successfully delivered metrics.
 *
 * If corruption in the network send provider's internal file(s) is detected,
 * this value will be reset to 0 and the calling code will be notified via
 * return values.
 *
 * This class abstracts away how and where this information is generated and
 * stored by providing a simple interface via the following functions:
 *
 * emer_network_send_provider_get_send_number()
 * emer_network_send_provider_increment_send_number()
 */

static void
set_network_send_path (EmerNetworkSendProvider *self,
                       const gchar             *given_path)
{
  EmerNetworkSendProviderPrivate *priv =
    emer_network_send_provider_get_instance_private (self);
  priv->path = g_strdup (given_path);
}

static void
emer_network_send_provider_set_property (GObject      *object,
                                         guint         property_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  EmerNetworkSendProvider *self = EMER_NETWORK_SEND_PROVIDER (object);

  switch (property_id)
    {
    case PROP_PATH:
      set_network_send_path (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
emer_network_send_provider_finalize (GObject *object)
{
  EmerNetworkSendProvider *self = EMER_NETWORK_SEND_PROVIDER (object);
  EmerNetworkSendProviderPrivate *priv =
    emer_network_send_provider_get_instance_private (self);

  g_free (priv->path);
  g_key_file_unref (priv->key_file);
  G_OBJECT_CLASS (emer_network_send_provider_parent_class)->finalize (object);
}

static void
emer_network_send_provider_class_init (EmerNetworkSendProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  /* Blurb string is good enough default documentation for this. */
  emer_network_send_provider_props[PROP_PATH] =
    g_param_spec_string ("path", "Path",
                         "The path to the file where the network send data is stored.",
                         DEFAULT_NETWORK_SEND_FILE_PATH,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  object_class->set_property = emer_network_send_provider_set_property;
  object_class->finalize = emer_network_send_provider_finalize;

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_network_send_provider_props);
}

static void
emer_network_send_provider_init (EmerNetworkSendProvider *self)
{
  EmerNetworkSendProviderPrivate *priv =
    emer_network_send_provider_get_instance_private (self);
  priv->key_file = g_key_file_new ();
}

/*
 * emer_network_send_provider_new:
 *
 * Constructs the provider used to obtain and store data regarding "network
 * send" metadata via the default filepath.
 *
 * Returns: (transfer full): A new #EmerNetworkSendProvider.
 * Free with g_object_unref().
 */
EmerNetworkSendProvider *
emer_network_send_provider_new (void)
{
  return g_object_new (EMER_TYPE_NETWORK_SEND_PROVIDER, NULL);
}

/*
 * emer_network_send_provider_new_full:
 * @path: path to a file containing network send data; see
 * #EmerNetworkSendProvider:path.
 *
 * Constructs the provider used to obtain and store data regarding "network
 * send" metadata via a given filepath.
 *
 * Returns: (transfer full): A new #EmerNetworkSendProvider.
 * Free with g_object_unref().
 */
EmerNetworkSendProvider *
emer_network_send_provider_new_full (const gchar *path)
{
  return g_object_new (EMER_TYPE_NETWORK_SEND_PROVIDER,
                       "path", path,
                       NULL);
}

static void
reset_network_send_data (EmerNetworkSendProvider *self)
{
  EmerNetworkSendProviderPrivate *priv =
    emer_network_send_provider_get_instance_private (self);

  g_key_file_set_integer (priv->key_file, NETWORK_SEND_GROUP,
                          NETWORK_SEND_KEY, 0);

  GError *error = NULL;
  if (!g_key_file_save_to_file (priv->key_file, priv->path, &error))
    {
      g_critical ("Failed to reset network send file. Error: %s.",
                  error->message);
      g_error_free (error);
    }
}

static gboolean
read_network_send_data (EmerNetworkSendProvider *self)
{
  EmerNetworkSendProviderPrivate *priv =
    emer_network_send_provider_get_instance_private (self);
  if (priv->data_cached)
    return TRUE;

  GError *error = NULL;
  if (!g_key_file_load_from_file (priv->key_file, priv->path, G_KEY_FILE_NONE,
                                  &error))
    goto handle_failed_read;

  priv->send_number = g_key_file_get_integer (priv->key_file,
                                              NETWORK_SEND_GROUP,
                                              NETWORK_SEND_KEY,
                                              &error);
  if (error != NULL)
    goto handle_failed_read;

  priv->data_cached = TRUE;
  return TRUE;

handle_failed_read:
  g_warning ("Failed to read from network send file. Resetting data. "
             "Error: %s.", error->message);
  g_error_free (error);
  reset_network_send_data (self);
  return FALSE;
}

/*
 * emer_network_send_provider_get_send_number:
 * @self: the network send provider.
 * @send_number: the address of the gint to store the send number in.
 *
 * Retrieves the network send number.
 *
 * Returns: a boolean indicating success or failure of retrieval.
 * If this returns %FALSE, 'send_number' will be unaltered.
 */
gboolean
emer_network_send_provider_get_send_number (EmerNetworkSendProvider *self,
                                            gint                    *send_number)
{
  EmerNetworkSendProviderPrivate *priv =
    emer_network_send_provider_get_instance_private (self);

  if (!read_network_send_data (self))
    return FALSE;

  *send_number = priv->send_number;
  return TRUE;
}

/*
 * emer_network_send_provider_increment_send_number:
 * @self: the network send provider.
 *
 * Increments the network send number and creates a new metadata file if one
 * doesn't already exist.
 *
 * Returns: %TRUE on success, and %FALSE on failure.
 */
gboolean
emer_network_send_provider_increment_send_number (EmerNetworkSendProvider *self)
{
  EmerNetworkSendProviderPrivate *priv =
    emer_network_send_provider_get_instance_private (self);

  if (!read_network_send_data (self))
    return FALSE;

  g_key_file_set_integer (priv->key_file, NETWORK_SEND_GROUP,
                          NETWORK_SEND_KEY, priv->send_number + 1);
  GError *error = NULL;
  if (!g_key_file_save_to_file (priv->key_file, priv->path, &error))
    {
      g_critical ("Failed to write to network send file. Error: %s.",
                  error->message);
      g_error_free (error);
      return FALSE;
    }

  priv->send_number++;
  return TRUE;
}
