/* Copyright 2013 Endless Mobile, Inc. */

#ifndef RUN_TESTS_H
#define RUN_TESTS_H

#include <glib.h>
#include <gio/gio.h>

#define TEST_LOG_DOMAIN "EosMetrics"

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

/* Each module adds its own test cases: */

void add_util_tests       (void);
void add_osversion_tests  (void);
void add_uuid_tests       (void);
void add_mac_tests        (void);
void add_web_tests        (void);
void add_connection_tests (void);
void add_sender_tests     (void);

#endif /* RUN_TESTS_H */
