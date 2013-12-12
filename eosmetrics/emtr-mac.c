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

/* Return an invalid MAC address: a 64-bit integer with the 49th bit set to 1,
since MAC addresses are 48 bits. */
static gint64
fake_mac_address (void)
{
  return (gint64)1 << 48;
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

/* Heuristically find out whether the platform is using a software-generated
MAC address */
static gboolean
is_address_software_generated (void)
{
  /* If the file /etc/smsc95xx_mac_addr is present, that indicates that we are
  on an ODROID U2, which doesn't have a hardware MAC address. The file contains
  a fake address, which is not useful for identifying the hardware. */
  GFile *fake_mac_file = g_file_new_for_path ("/etc/smsc95xx_mac_addr");
  gboolean retval = g_file_query_exists (fake_mac_file, NULL);
  g_object_unref (fake_mac_file);
  return retval;
}

/*
 * emtr_mac_gen:
 *
 * Retrieve the MAC address of one of this machine's network interfaces
 * (<quote>hardware address</quote>).
 * The network interface eth0 is used if it is available, or else any other
 * network interface that is not the loopback interface.
 *
 * <note><para>
 *   Do not rely on the function preferring eth0, or on network interfaces
 *   being present or absent; the function is internal to the library, so
 *   it can do whatever is necessary to identify the hardware.
 *   In particular, do not treat this as a way to get the MAC address of eth0.
 * </para></note>
 *
 * This should uniquely identify the hardware; however, some platforms do not
 * have a hardware MAC address, and generate one on first boot.
 * This algorithm tries to determine if that is the case; if so, it returns all
 * zero bits with a 1 in the 49th bit, which is not a valid MAC address.
 * This value represents <quote>unidentifiable hardware</quote>
 *
 * Returns: a valid MAC address as a 48-bit integer, or the value
 * 0x01000000000000 if a uniquely identifying MAC address could not be found.
 */
gint64
emtr_mac_gen (void)
{
  static gint64 cached_mac_address = -1;

  if (cached_mac_address != -1)
    return cached_mac_address;

  if (is_address_software_generated ())
    {
      g_debug ("On a platform with software-generated MAC address");
      cached_mac_address = fake_mac_address ();
      return cached_mac_address;
    }

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
