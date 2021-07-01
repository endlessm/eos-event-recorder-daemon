/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2014 - 2016 Endless Mobile, Inc. */

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

#include "emer-boot-id-provider.h"

#include <string.h>
#include <uuid/uuid.h>

typedef struct EmerBootIdProviderPrivate
{
  gchar *path;
  uuid_t id;
} EmerBootIdProviderPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (EmerBootIdProvider, emer_boot_id_provider, G_TYPE_OBJECT)

#define BOOT_ID_PROVIDER_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), EMER_TYPE_BOOT_ID_PROVIDER, EmerBootIdProviderPrivate))

/*
 * The expected size in bytes of the file located at
 * /proc/sys/kernel/random/boot_id. The file should be 32 lower-case hexadecimal
 * characters interspersed with 4 hyphens and terminated with a newline
 * character.
 *
 * Exact format: "%08x-%04x-%04x-%04x-%012x\n"
 */
#define FILE_LENGTH 37

/*
 * The filepath to the system file containing a statistically unique identifier
 * (uuid) for the current boot of the machine. Will vary from boot to boot.
 */
#define DEFAULT_BOOT_ID_FILEPATH "/proc/sys/kernel/random/boot_id"

enum
{
  PROP_0,
  PROP_PATH,
  NPROPS
};

static GParamSpec *emer_boot_id_provider_props[NPROPS] = { NULL, };

/*
 * SECTION:emer-boot-id-provider
 * @title: Boot ID Provider
 * @short_description: Provides unique boot identifiers.
 *
 * The boot ID provider supplies UUIDs which uniquely identify each boot of the
 * computer.
 *
 * This class abstracts away how and where UUIDs are generated from by providing
 * a simple interface via emer_boot_id_provider_get_id() to whatever calling
 * code needs it.
 */

static const gchar *
get_id_path (EmerBootIdProvider *self)
{
  EmerBootIdProviderPrivate *priv =
    emer_boot_id_provider_get_instance_private (self);
  return priv->path;
}

static void
set_id_path (EmerBootIdProvider *self,
             const gchar        *given_path)
{
  EmerBootIdProviderPrivate *priv =
    emer_boot_id_provider_get_instance_private (self);
  priv->path = g_strdup (given_path);
}

static void
emer_boot_id_provider_get_property (GObject    *object,
                                    guint       property_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  EmerBootIdProvider *self = EMER_BOOT_ID_PROVIDER (object);

  switch (property_id)
    {
    case PROP_PATH:
      g_value_set_string (value, get_id_path (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_boot_id_provider_set_property (GObject      *object,
                                    guint         property_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  EmerBootIdProvider *self = EMER_BOOT_ID_PROVIDER (object);

  switch (property_id)
    {
    case PROP_PATH:
        set_id_path (self, g_value_get_string (value));
        break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
emer_boot_id_provider_finalize (GObject *object)
{
  EmerBootIdProvider *self = EMER_BOOT_ID_PROVIDER (object);
  EmerBootIdProviderPrivate *priv =
    emer_boot_id_provider_get_instance_private (self);
  g_free (priv->path);

  G_OBJECT_CLASS (emer_boot_id_provider_parent_class)->finalize (object);
}

static void
emer_boot_id_provider_class_init (EmerBootIdProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = emer_boot_id_provider_get_property;
  object_class->set_property = emer_boot_id_provider_set_property;
  object_class->finalize = emer_boot_id_provider_finalize;

  /* Blurb string is good enough default documentation for this */
  emer_boot_id_provider_props[PROP_PATH] =
    g_param_spec_string ("path", "Path",
                         "The path to the file where the unique identifier is stored.",
                         DEFAULT_BOOT_ID_FILEPATH,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NPROPS,
                                     emer_boot_id_provider_props);
}

// Mandatory empty function.
static void
emer_boot_id_provider_init (EmerBootIdProvider *self)
{
}

/*
 * emer_boot_id_provider_new:
 *
 * Gets the ID provider used to obtain a unique boot ID via the default
 * filepath.
 *
 * Returns: (transfer full): A new #EmerBootIdProvider.
 * Free with g_object_unref().
 */
EmerBootIdProvider *
emer_boot_id_provider_new (void)
{
  return g_object_new (EMER_TYPE_BOOT_ID_PROVIDER,
                       "path", DEFAULT_BOOT_ID_FILEPATH,
                       NULL);
}

/*
 * emer_boot_id_provider_new_full:
 * @boot_id_file_path: path to a file; see #EmerBootIdProvider:path
 *
 * Gets the ID provider used to obtain a unique boot ID via a given filepath.
 *
 * Returns: (transfer full): A new #EmerBootIdProvider.
 * Free with g_object_unref().
 */
EmerBootIdProvider *
emer_boot_id_provider_new_full (const gchar *boot_id_file_path)
{
  return g_object_new (EMER_TYPE_BOOT_ID_PROVIDER,
                       "path", boot_id_file_path,
                       NULL);
}

static gboolean
read_boot_id (EmerBootIdProvider *self)
{
  EmerBootIdProviderPrivate *priv =
    emer_boot_id_provider_get_instance_private (self);

  gchar *boot_id_string;
  gsize boot_id_length;
  GError *error = NULL;
  gboolean read_succeeded =
    g_file_get_contents (priv->path, &boot_id_string, &boot_id_length, &error);

  if (!read_succeeded)
    {
      g_critical ("Failed to read boot ID file (%s). Error: %s.", priv->path,
                  error->message);
      g_error_free (error);
      return FALSE;
    }

  if (strlen (boot_id_string) != boot_id_length)
    {
      g_critical ("Boot ID file (%s) contained null byte, but should be "
                  "hexadecimal.", priv->path);
      return FALSE;
    }

  if (boot_id_length != FILE_LENGTH)
    {
      g_critical ("Boot ID file (%s) contained %" G_GSIZE_FORMAT " bytes, "
                  "but expected %d bytes.",
                  priv->path, boot_id_length, FILE_LENGTH);
      return FALSE;
    }

  // Remove newline.
  g_strchomp (boot_id_string);

  gint parse_failed = uuid_parse (boot_id_string, priv->id);
  g_free (boot_id_string);

  if (parse_failed != 0)
    {
      g_critical ("Boot ID file (%s) did not contain UUID.", priv->path);
      return FALSE;
    }

  return TRUE;
}

/*
 * emer_boot_id_provider_get_id:
 * @self: the boot ID provider
 * @uuid: (out caller-allocates) (array fixed-size=16) (element-type guchar):
 * allocated 16-byte return location for a UUID.
 *
 * Retrieves an ID (in the form of a UUID) that is unique to this boot, for
 * use in anonymously identifying metrics data.
 *
 * Returns: a boolean indicating success or failure of retrieval.
 * If this returns %FALSE, the UUID cannot be trusted to be valid.
 */
gboolean
emer_boot_id_provider_get_id (EmerBootIdProvider *self,
                              uuid_t              uuid)
{
  EmerBootIdProviderPrivate *priv =
    emer_boot_id_provider_get_instance_private (self);
  static gboolean id_is_valid = FALSE;

  if (!id_is_valid)
    {
      if (!read_boot_id (self))
        return FALSE;
    }

  uuid_copy (uuid, priv->id);
  id_is_valid = TRUE;
  return TRUE;
}

#define KERNEL_CMDLINE_PATH	"/proc/cmdline"
#define LIVE_BOOT_FLAG_REGEX	"\\bendless\\.live_boot\\b"
#define DUAL_BOOT_FLAG_REGEX	"\\bendless\\.image\\.device\\b"

#define NORMAL_BOOT	0x0
#define DUAL_BOOT	0x1
#define LIVE_BOOT	0x2

/*
 * emer_boot_id_provider_get_boot_type:
 *
 * Get boot type by checking kernel's boot command line.
 *
 * Returns: a number 0x0 for normal boot, 0x1 for dual boot, 0x2 for live boot.
 */
guint8
emer_boot_id_provider_get_boot_type (void)
{
  g_autofree gchar *cmdline = NULL;
  g_autoptr(GError) error = NULL;
  static guint8 boot_type = NORMAL_BOOT;
  static gboolean boot_type_is_cached = FALSE;

  if (boot_type_is_cached)
    return boot_type;

  /* Endless OS places the boot type in kernel's boot command line */
  if (!g_file_get_contents (KERNEL_CMDLINE_PATH, &cmdline, NULL, &error))
    {
      g_warning ("Error reading " KERNEL_CMDLINE_PATH ": %s", error->message);
    }
  else if (g_regex_match_simple (DUAL_BOOT_FLAG_REGEX, cmdline, 0, 0))
    {
      boot_type = DUAL_BOOT;
    }
  else if (g_regex_match_simple (LIVE_BOOT_FLAG_REGEX, cmdline, 0, 0))
    {
      boot_type = LIVE_BOOT;
    }

  boot_type_is_cached = TRUE;

  return boot_type;
}
