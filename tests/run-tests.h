/* Copyright 2013 Endless Mobile, Inc. */

#ifndef RUN_TESTS_H
#define RUN_TESTS_H

#include <glib.h>
#include <gio/gio.h>

#define TEST_LOG_DOMAIN "EosMetrics"

/* Common code for tests */

gboolean  mock_web_send           (const gchar  *uri,
                                   const gchar  *data,
                                   const gchar  *username,
                                   const gchar  *password,
                                   GCancellable *cancellable,
                                   GError      **error);

gboolean  mock_web_send_exception (const gchar  *uri,
                                   const gchar  *data,
                                   const gchar  *username,
                                   const gchar  *password,
                                   GCancellable *cancellable,
                                   GError      **error);

GVariant *create_payload          (const gchar  *message,
                                   gint64        timestamp,
                                   gboolean      is_bug);

/* Each module adds its own test cases: */

void add_util_tests       (void);
void add_osversion_tests  (void);
void add_uuid_tests       (void);
void add_mac_tests        (void);
void add_web_tests        (void);
void add_connection_tests (void);
void add_sender_tests     (void);

#endif /* RUN_TESTS_H */
