/* Copyright 2013 Endless Mobile, Inc. */

#ifndef RUN_TESTS_H
#define RUN_TESTS_H

#include <glib.h>
#include <gio/gio.h>

#define TEST_LOG_DOMAIN "EosMetrics"
#define MOCK_VERSION_FILE_ENVVAR "_MOCK_ENDLESSOS_VERSION_FILE"
#define MOCK_VERSION_FILE_CONTENTS \
  "<endlessos-version>\n" \
  "  <platform>1</platform>\n" \
  "  <minor>2</minor>\n" \
  "  <micro>0</micro>\n" \
  "  <distributor>Endless Mobile</distributor>\n" \
  "  <date>2013-11-27</date>\n" \
  "</endlessos-version>"

/* Common code for tests */

gboolean  mock_web_send_sync            (const gchar        *uri,
                                         const gchar        *data,
                                         const gchar        *username,
                                         const gchar        *password,
                                         GCancellable       *cancellable,
                                         GError            **error);

void      mock_web_send_async           (const gchar        *uri,
                                         const gchar        *data,
                                         const gchar        *username,
                                         const gchar        *password,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            callback_data);

gboolean  mock_web_send_finish          (GAsyncResult       *result,
                                         GError            **error);

gboolean  mock_web_send_exception_sync  (const gchar        *uri,
                                         const gchar        *data,
                                         const gchar        *username,
                                         const gchar        *password,
                                         GCancellable       *cancellable,
                                         GError            **error);


void      mock_web_send_exception_async (const gchar        *uri,
                                         const gchar        *data,
                                         const gchar        *username,
                                         const gchar        *password,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            callback_data);

GVariant *create_payload                (const gchar       *message,
                                         gint64             timestamp,
                                         gboolean           is_bug);

void      set_up_mock_version_file      (const gchar       *contents);

/* Each module adds its own test cases: */

void add_util_tests       (void);
void add_osversion_tests  (void);
void add_uuid_tests       (void);
void add_mac_tests        (void);
void add_web_tests        (void);
void add_connection_tests (void);
void add_sender_tests     (void);

#endif /* RUN_TESTS_H */
