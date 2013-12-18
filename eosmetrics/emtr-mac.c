/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2013 Endless Mobile, Inc. */

#include "emtr-mac-private.h"

#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <gio/gio.h>

/*
 * SECTION:mac
 * Facility for retrieving the machine's MAC address
 */

/* Return -1 if the string was not in standard format and could not be parsed */
static gint64
parse_mac_address (const gchar *mac_string)
{
  unsigned char bytes[6];
  int nbytes = sscanf (mac_string, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                       bytes, bytes + 1, bytes + 2,
                       bytes + 3, bytes + 4, bytes + 5);
  if (nbytes != 6)
    return -1;
  return (gint64)bytes[5]
         | (gint64)bytes[4] << 8
         | (gint64)bytes[3] << 16
         | (gint64)bytes[2] << 24
         | (gint64)bytes[1] << 32
         | (gint64)bytes[0] << 40;
}

/* Return a random 48-bit number with its 8th bit set to 1, as recommended in
RFC 4122 */
static gint64
fake_mac_address (void)
{
  return (gint64)g_random_int ()
         | (gint64)g_random_int_range (0, 1 << 16) << 32
         | (gint64)1 << 40;
}

/* Pass a GFile referring to something like "/sys/class/net/eth0" and this will
return the MAC address as a newly allocated string, or NULL on failure. */
static gchar *
get_mac_string_from_sysfs_file (GFile   *file,
                                GError **error)
{
  GFile *sysfs_address_file = g_file_get_child (file, "address");
  gchar *address;
  gboolean success = g_file_load_contents (sysfs_address_file, NULL, &address,
                                           NULL, NULL, error);
  g_object_unref (sysfs_address_file);
  if (!success)
    return NULL;
  return address;
}

/*
 * emtr_mac_gen:
 *
 * Retrieve the MAC address of this machine's network interface (<quote>hardware
 * address</quote>).
 *
 * Returns: the MAC address as a 48-bit positive integer.
 */
gint64
emtr_mac_gen (void)
{
  static gint64 cached_mac_address = -1;

  if (cached_mac_address != -1)
    return cached_mac_address;

  /* This relies on the underlying distribution using sysfs (kernel > 2.5). If
  ever that assumption becomes incorrect, there are other ways: using ioctl
  (http://stackoverflow.com/a/1779758), and using NetworkManager. */
  GError *error = NULL;
  GFile *sysfs_interfaces_dir = g_file_new_for_path ("/sys/class/net");
  GFileEnumerator *interfaces = g_file_enumerate_children (sysfs_interfaces_dir,
                                                           G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                           G_FILE_QUERY_INFO_NONE,
                                                           NULL, &error);
  if (interfaces == NULL)
    {
      g_warning ("Could not determine MAC address: %s", error->message);
      g_error_free (error);
      g_free (sysfs_interfaces_dir);
      goto fail;
    }

  /* Iterate over all network addresses until we find one with a MAC address */
  GFileInfo *info = NULL;
  gchar *mac_string = NULL;
  while ((info = g_file_enumerator_next_file (interfaces, NULL, &error)))
    {
      const gchar *interface_name = g_file_info_get_name (info);
      /* Skip the loopback interface */
      if (strcmp (interface_name, "lo") == 0)
        {
          g_object_unref (info);
          continue;
        }
      /* Otherwise, get the MAC address of this interface if it is the best one
      we have so far (prefer eth0) */
      if (mac_string == NULL || strcmp (interface_name, "eth0") == 0)
        {
          GError *inner_error = NULL;
          GFile *interface_dir = g_file_enumerator_get_child (interfaces, info);
          gchar *new_string = get_mac_string_from_sysfs_file (interface_dir,
                                                              &inner_error);
          g_object_unref (interface_dir);
          if (new_string != NULL)
            {
              g_free (mac_string);
              mac_string = new_string;
              g_debug ("Loaded MAC address from %s", interface_name);
            }
          else
            {
              g_debug ("Failed to load MAC address from %s: %s", interface_name,
                       inner_error->message);
              g_error_free (inner_error);
            }
        }
      g_object_unref (info);
    }
  g_file_enumerator_close (interfaces, NULL, NULL);
  g_object_unref (interfaces);
  g_object_unref (sysfs_interfaces_dir);

  if (error != NULL)
    {
      g_warning ("Could not determine MAC address: %s", error->message);
      g_error_free (error);
      goto fail;
    }

  g_assert (mac_string);

  gint64 parsed_mac_address = parse_mac_address (mac_string);
  if (parsed_mac_address == -1)
    {
      g_warning ("Could not parse MAC address string %s", mac_string);
      g_free (mac_string);
      goto fail;
    }
  g_free (mac_string);

  cached_mac_address = parsed_mac_address;
  return cached_mac_address;

fail:
  g_warning ("Using fake MAC address\n");
  cached_mac_address = fake_mac_address ();
  return cached_mac_address;
}
