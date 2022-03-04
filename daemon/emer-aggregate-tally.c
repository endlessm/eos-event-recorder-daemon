/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* Copyright 2021 Endless OS Foundation, LLC */

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

#include "config.h"
#include "emer-aggregate-tally.h"
#include "shared/metrics-util.h"

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sqlite3.h>

struct _EmerAggregateTally
{
  GObject parent_instance;

  gchar *persistent_cache_directory;
  sqlite3 *db;
};

G_DEFINE_TYPE (EmerAggregateTally, emer_aggregate_tally, G_TYPE_OBJECT)

/* Error domain; codes from https://www.sqlite.org/rescode.html */
G_DEFINE_QUARK ("emer-sqlite-error", emer_sqlite_error)
#define EMER_SQLITE_ERROR (emer_sqlite_error_quark ())

enum {
  PROP_PERSISTENT_CACHE_DIRECTORY = 1,
  N_PROPS,
};

static GParamSpec *properties[N_PROPS] = { NULL, };

static gchar *
format_datetime_for_tally_type (GDateTime     *datetime,
                                EmerTallyType  tally_type)
{
  switch (tally_type)
    {
    case EMER_TALLY_DAILY_EVENTS:
      return g_date_time_format (datetime, "%Y-%m-%d");

    case EMER_TALLY_MONTHLY_EVENTS:
      return g_date_time_format (datetime, "%Y-%m");

    default:
      g_assert_not_reached ();
    }

  return NULL;
}

static void
ensure_folder_exists (EmerAggregateTally  *self,
                      const char          *path,
                      GError             **error)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(GFile) file = NULL;

  file = g_file_new_for_path (path);
  g_file_make_directory_with_parents (file, NULL, &local_error);

  if (local_error && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_propagate_error (error, g_steal_pointer (&local_error));
}

static gboolean
check_sqlite_error (const char          *tag,
                    int                  ret,
                    GError             **error)
{
  if (ret == SQLITE_OK || ret == SQLITE_DONE)
    return TRUE;

  g_set_error (error,
               EMER_SQLITE_ERROR,
               ret,
               "%s: %s",
               tag,
               sqlite3_errstr (ret));
  return FALSE;
}

#define CHECK(x) \
  check_sqlite_error ((G_STRLOC), (x), error)

static void
column_to_uuid (sqlite3_stmt *stmt,
                int           i,
                uuid_t        u)
{
  gconstpointer blob = sqlite3_column_blob (stmt, i);
  gint size = sqlite3_column_bytes (stmt, i);

  if (blob != NULL && size == sizeof (uuid_t))
    uuid_copy (u, blob);
  else
    g_warning ("Malformed UUID of size %d", size);
}

static GVariant *
column_to_variant (sqlite3_stmt *stmt,
                   int           i)
{
  g_autoptr(GVariant) variant = NULL;
  gconstpointer sqlite_data;
  gint size;

  /* Unlike sqlite3_column_text, "The return value from sqlite3_column_blob()
   * for a zero-length BLOB is a NULL pointer." even if the value is not NULL
   * at the SQL level. Weird, but it suits our purposes of storing the
   * absence of a payload as a non-NULL empty blob.
   */
  sqlite_data = sqlite3_column_blob (stmt, i);
  if (!sqlite_data)
    return NULL;

  size = sqlite3_column_bytes (stmt, i);
  gpointer data = g_memdup (sqlite_data, size);

  variant = g_variant_new_from_data (G_VARIANT_TYPE_VARIANT,
                                     data, size, FALSE,
                                     g_free, data);
  return swap_bytes_if_big_endian (g_variant_ref_sink (variant));
}

static guint32
column_to_uint32 (sqlite3_stmt *stmt,
                  int           i)
{
  sqlite_int64 number = sqlite3_column_int64 (stmt, i);
  return CLAMP (number, 0, G_MAXUINT32);
}

G_STATIC_ASSERT (sizeof (sqlite_int64) == sizeof (gint64));

static gboolean
delete_tally_entries (EmerAggregateTally  *self,
                      GArray              *rows_to_delete,
                      GError             **error)
{
  const char *DELETE_SQL = "DELETE FROM tally WHERE id IN ";
  g_autoptr(GString) query = NULL;
  sqlite3_stmt *stmt = NULL;
  size_t i;

  if (!rows_to_delete || rows_to_delete->len == 0)
    return TRUE;

  query = g_string_new (DELETE_SQL);
  g_string_append (query, "(");
  for (i = 0; i < rows_to_delete->len; i++)
    {
      if (i > 0)
        g_string_append (query, ", ");

      sqlite_int64 row_id = g_array_index (rows_to_delete, sqlite_int64, i);
      g_string_append_printf (query, "%" G_GINT64_FORMAT, (gint64) row_id);
    }
  g_string_append (query, ");");

  if (!CHECK (sqlite3_prepare_v2 (self->db, query->str, -1, &stmt, NULL)) ||
      !CHECK (sqlite3_step (stmt)) ||
      !CHECK (sqlite3_reset (stmt)) ||
      !CHECK (sqlite3_finalize (stmt)))
    {
      g_prefix_error (error, "Failed to delete %u tally entries: ", rows_to_delete->len);
      return FALSE;
    }

  return TRUE;
}

static void
close_db (sqlite3 *db)
{
  g_autoptr(GError) error = NULL;

  if (db != NULL)
    return;

  if (!check_sqlite_error ("sqlite3_close", sqlite3_close (db), &error))
    g_warning ("Failed to close database: %s", error->message);
}

static sqlite3_int64
emer_aggregate_tally_read_user_version (sqlite3  *db,
                                        GError  **error)
{
  sqlite3_stmt *stmt = NULL;
  sqlite3_int64 version = 0;
  int ret = 0;

  if (!CHECK (sqlite3_prepare_v2 (db, "PRAGMA user_version", -1, &stmt, NULL)))
    return -1;

  ret = sqlite3_step (stmt);
  if (ret == SQLITE_ROW)
    version = sqlite3_column_int64 (stmt, 0);
  else if (!CHECK (ret))
    return -1;

  if (!CHECK (sqlite3_finalize (stmt)))
    return -1;

  return version;
}

static gboolean
emer_aggregate_tally_init_db (EmerAggregateTally  *self,
                              const char          *path,
                              GError             **error)
{
  g_assert (self->db == NULL);

  if (!CHECK (sqlite3_open (path, &self->db)))
    {
      g_prefix_error (error, "Failed to open %s: ", path);
      return FALSE;
    }

  g_assert (self->db);

  if (!CHECK (sqlite3_extended_result_codes (self->db, TRUE)))
    return FALSE;

  /* Use write-ahead logging rather than the default rollback journal. WAL
   * reduces the number of writes to disk, and crucially only calls fsync()
   * intermittently.
   *
   * TODO: should we force a checkpoint/fsync() to disk periodically, e.g.
   * whenever we try to submit to the server?
   *
   * https://sqlite.org/wal.html
   */
  if (!CHECK (sqlite3_exec (self->db, "PRAGMA journal_mode = WAL", NULL, NULL, NULL)))
    return FALSE;

  /* Magic number is "emer" in ASCII */
  if (!CHECK (sqlite3_exec (self->db, "PRAGMA application_id = 0x656d6572", NULL, NULL, NULL)))
    return FALSE;

  sqlite3_int64 user_version = emer_aggregate_tally_read_user_version (self->db, error);
  g_debug ("Current user_version for %s: %" G_GINT64_FORMAT, path, (gint64) user_version);

  switch (user_version)
    {
    case -1:
      /* Indicates an error from the user_version query above. */
      return FALSE;

    case 0:
      /* New, empty database. Just create the desired final schema. */
      if (!CHECK (sqlite3_exec (self->db,
                                "CREATE TABLE IF NOT EXISTS tally (\n"
                                "    id INTEGER PRIMARY KEY ASC,\n"
                                "    date TEXT NOT NULL,\n"
                                "    event_id BLOB NOT NULL CHECK (length(event_id) = 16),\n"
                                "    unix_user_id INT NOT NULL,\n"
                                "    payload BLOB NOT NULL,\n"
                                "    counter INT NOT NULL\n"
                                ")",
                                NULL, NULL, NULL)) ||
          !CHECK (sqlite3_exec (self->db,
                                "CREATE UNIQUE INDEX IF NOT EXISTS "
                                "ix_tally_unique_fields ON tally (\n"
                                "    date,\n"
                                "    event_id,\n"
                                "    unix_user_id,\n"
                                "    payload\n"
                                ")",
                                NULL, NULL, NULL)) ||
          !CHECK (sqlite3_exec (self->db, "PRAGMA user_version = 2", NULL, NULL, NULL)))
        return FALSE;

      return TRUE;

    case 1:
      /* This version of the schema had an aggregate_key column. Unfortunately
       * the SQLite library version in the current Endless OS does not support
       * ALTER TABLE _ DROP COLUMN _. Happily this schema version did not
       * enter a released version of the OS.
       */
      g_set_error (error, EMER_SQLITE_ERROR, SQLITE_ERROR,
                   "Can't migrate from schema version 1");
      return FALSE;

    /* If future schema changes can be done non-destructively, add
     * additional cases to ALTER TABLE etc. to make the existing data
     * match the 'case 0' schema above.
     */
    case 2:
      return TRUE;

    default:
      g_set_error (error, EMER_SQLITE_ERROR, SQLITE_ERROR,
                   "Unexpected user_version %" G_GINT64_FORMAT,
                   (gint64) user_version);
      return FALSE;
    }
}

static void
emer_aggregate_tally_delete_db (EmerAggregateTally *self,
                                const char         *db_path)
{
  /*
   * When in active use, the state of a WAL mode database is described by
   * three separate files:
   *
   * 1. The main database file with an arbitrary name "X".
   * 2. The write-ahead log file, usually named "X-wal".
   * 3. The wal-index file, usually named "X-shm".
   *
   *  -- https://sqlite.org/walformat.html
   */
  const gchar *suffixes[] = {
    "",
    "-shm",
    "-wal"
  };

  for (size_t i = 0; i < G_N_ELEMENTS (suffixes); i++)
    {
      g_autofree gchar *path = g_strconcat (db_path, suffixes[i], NULL);

      if (0 != g_unlink (path) && errno != ENOENT)
        g_warning ("Failed to unlink %s: %s", path, g_strerror (errno));
    }
}

static void
emer_aggregate_tally_constructed (GObject *object)
{
  EmerAggregateTally *self = EMER_AGGREGATE_TALLY (object);
  g_autofree gchar *old_path = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr(GError) error = NULL;

  G_OBJECT_CLASS (emer_aggregate_tally_parent_class)->constructed (object);

  g_assert (self->persistent_cache_directory != NULL);
  ensure_folder_exists (self, self->persistent_cache_directory, NULL);

 /* No daemon version that used this path made it into a production release,
  * or even beta. So rather than migrating the data for the schema & filename
  * change, just delete it (and ignore errors).
  */
  old_path = g_build_filename (self->persistent_cache_directory,
                               "aggregate-events.db",
                               NULL);
  emer_aggregate_tally_delete_db (self, old_path);

  path = g_build_filename (self->persistent_cache_directory,
                           "metrics.db",
                           NULL);
  if (!emer_aggregate_tally_init_db (self, path, &error))
    {
      g_warning ("Failed to initialize %s: %s; trying to delete and recreate it",
                 path, error->message);
      g_clear_error (&error);

      g_clear_pointer (&self->db, close_db);
      emer_aggregate_tally_delete_db (self, path);

      if (!emer_aggregate_tally_init_db (self, path, &error))
        {
          g_error ("Failed to initialize %s again: %s", path, error->message);
        }
    }
}

static void
emer_aggregate_tally_finalize (GObject *object)
{
  EmerAggregateTally *self = (EmerAggregateTally *)object;

  g_clear_pointer (&self->db, close_db);
  g_clear_pointer (&self->persistent_cache_directory, g_free);

  G_OBJECT_CLASS (emer_aggregate_tally_parent_class)->finalize (object);
}

static void
emer_aggregate_tally_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  EmerAggregateTally *self = EMER_AGGREGATE_TALLY (object);

  switch (prop_id)
    {
    case PROP_PERSISTENT_CACHE_DIRECTORY:
      g_assert (self->persistent_cache_directory == NULL);
      self->persistent_cache_directory = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
emer_aggregate_tally_class_init (EmerAggregateTallyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = emer_aggregate_tally_constructed;
  object_class->finalize = emer_aggregate_tally_finalize;
  object_class->set_property = emer_aggregate_tally_set_property;

  /*
   * EmerDaemon:persistent-cache-directory:
   *
   * A directory for temporarily storing events until they are uploaded to the
   * metrics servers.
   */
  properties[PROP_PERSISTENT_CACHE_DIRECTORY] =
    g_param_spec_string ("persistent-cache-directory",
                         "Persistent cache directory",
                         "The directory in which to temporarily store events "
                         "locally",
                         NULL,
                         G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE |
                         G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
emer_aggregate_tally_init (EmerAggregateTally *self)
{
}

EmerAggregateTally *
emer_aggregate_tally_new (const char *persistent_cache_directory)
{
  return g_object_new (EMER_TYPE_AGGREGATE_TALLY,
                       "persistent-cache-directory", persistent_cache_directory,
                       NULL);
}

gboolean
emer_aggregate_tally_store_event (EmerAggregateTally  *self,
                                  EmerTallyType        tally_type,
                                  guint32              unix_user_id,
                                  uuid_t               event_id,
                                  GVariant            *payload,
                                  guint32              counter,
                                  GDateTime           *datetime,
                                  GError             **error)
{
  g_return_val_if_fail (payload == NULL || g_variant_is_of_type (payload, G_VARIANT_TYPE_VARIANT), FALSE);

  const char *UPSERT_SQL =
    "INSERT INTO tally (date, event_id, unix_user_id, "
    "                   payload, counter) "
    "VALUES (?, ?, ?, ?, ?) "
    "ON CONFLICT (date, event_id, unix_user_id, "
    "             payload) "
    "DO UPDATE SET counter = tally.counter + excluded.counter;";

  g_autofree gchar *date = NULL;
  sqlite3_stmt *stmt = NULL;

  date = format_datetime_for_tally_type (datetime, tally_type);

  return
      CHECK (sqlite3_prepare_v2 (self->db, UPSERT_SQL, -1, &stmt, NULL)) &&
      CHECK (sqlite3_bind_text (stmt, 1, date, -1, SQLITE_TRANSIENT)) &&
      CHECK (sqlite3_bind_blob (stmt, 2, event_id, sizeof (uuid_t), SQLITE_STATIC)) &&
      CHECK (sqlite3_bind_int64 (stmt, 3, unix_user_id)) &&
      CHECK (sqlite3_bind_blob (stmt, 4,
                                payload ? g_variant_get_data (payload) : "",
                                payload ? g_variant_get_size (payload) : 0,
                                SQLITE_TRANSIENT)) &&
      CHECK (sqlite3_bind_int64 (stmt, 5, counter)) &&
      CHECK (sqlite3_step (stmt)) &&
      CHECK (sqlite3_finalize (stmt));
}

G_STATIC_ASSERT (sizeof (sqlite3_int64) == sizeof (gint64));

static gboolean
emer_aggregate_tally_iter_internal (EmerAggregateTally *self,
                                    const char         *query,
                                    EmerTallyType       tally_type,
                                    GDateTime          *datetime,
                                    EmerTallyIterFlags  flags,
                                    EmerTallyIterFunc   func,
                                    gpointer            user_data,
                                    GError             **error)
{
  g_autoptr(GArray) rows_to_delete = NULL;
  g_autofree gchar *date = NULL;
  sqlite3_stmt *stmt = NULL;
  int ret;

  date = format_datetime_for_tally_type (datetime, tally_type);
  rows_to_delete = g_array_new (FALSE, FALSE, sizeof (sqlite3_int64));

  if (!CHECK (sqlite3_prepare_v2 (self->db, query, -1, &stmt, NULL)) ||
      !CHECK (sqlite3_bind_text (stmt, 1, date, -1, SQLITE_TRANSIENT)))
    {
      g_prefix_error (error, "While preparing query: ");
      return FALSE;
    }

  while ((ret = sqlite3_step (stmt)) == SQLITE_ROW)
    {
      guint32 unix_user_id = column_to_uint32 (stmt, 2);
      g_autoptr(GVariant) payload = column_to_variant (stmt, 3);
      guint32 counter = column_to_uint32 (stmt, 4);
      const char *event_date = (const char *) sqlite3_column_text (stmt, 5);
      uuid_t event_id = { 0 };
      EmerTallyIterResult result;

      column_to_uuid (stmt, 1, event_id);

      result = func (unix_user_id, event_id,
                     payload,
                     counter, event_date, user_data);

      if (flags & EMER_TALLY_ITER_FLAG_DELETE)
        {
          const sqlite3_int64 row_id = sqlite3_column_int64 (stmt, 0);
          g_array_append_val (rows_to_delete, row_id);
        }

      if (result & EMER_TALLY_ITER_STOP)
        break;
    }

  return
      CHECK (ret) &&
      CHECK (sqlite3_finalize (stmt)) &&
      delete_tally_entries (self, rows_to_delete, error);
}

void
emer_aggregate_tally_iter (EmerAggregateTally *self,
                           EmerTallyType       tally_type,
                           GDateTime          *datetime,
                           EmerTallyIterFlags  flags,
                           EmerTallyIterFunc   func,
                           gpointer            user_data)
{
  const char *SELECT_SQL =
    "SELECT id, event_id, unix_user_id, "
    "       payload, counter, date "
    "FROM tally "
    "WHERE date = ?";
  g_autoptr(GError) error = NULL;

  if (!emer_aggregate_tally_iter_internal (self,
                                           SELECT_SQL,
                                           tally_type,
                                           datetime,
                                           flags,
                                           func,
                                           user_data,
                                           &error))
    g_critical ("%s: %s", G_STRFUNC, error->message);
}

void
emer_aggregate_tally_iter_before (EmerAggregateTally *self,
                                  EmerTallyType       tally_type,
                                  GDateTime          *datetime,
                                  EmerTallyIterFlags  flags,
                                  EmerTallyIterFunc   func,
                                  gpointer            user_data)
{
  const char *SELECT_SQL =
    "SELECT id, event_id, unix_user_id, "
    "       payload, counter, date "
    "FROM tally "
    "WHERE length(date) = length(?1) AND date < ?1;";
  g_autoptr(GError) error = NULL;

  if (!emer_aggregate_tally_iter_internal (self,
                                           SELECT_SQL,
                                           tally_type,
                                           datetime,
                                           flags,
                                           func,
                                           user_data,
                                           &error))
    g_critical ("%s: %s", G_STRFUNC, error->message);
}
