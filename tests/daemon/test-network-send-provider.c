/* Copyright 2014 Endless Mobile, Inc. */

#include "emer-network-send-provider.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#define TESTING_FILE_PATH "testing_network_send_XXXXXX"

#define RESET_SEND_NUMBER 0

#define STARTING_SEND_NUMBER 42
#define STARTING_KEY_FILE \
  "[network_send_data]\n" \
  "network_requests_sent=42\n"

#define OTHER_KEY_FILE \
  "[network_send_data]\n" \
  "network_requests_sent=999\n"

#define INVALID_KEY_FILE \
  "[hungry_hungry_hippos]\n" \
  "marbles=-12\n" \
  "wicked_laughter=Mwahahahahahaha\n" \
  "evil=TRUE\n"

// Helper Functions

typedef struct Fixture
{
  EmerNetworkSendProvider *network_send_provider;
  GFile *tmp_file;
  gchar *tmp_path;
  GKeyFile *key_file;
} Fixture;

static void
write_testing_keyfile (Fixture       *fixture,
                       const gchar   *key_file_data)
{
  GError *error = NULL;
  g_key_file_load_from_data (fixture->key_file, key_file_data, -1,
                             G_KEY_FILE_NONE, &error);
  g_assert_no_error (error);

  g_key_file_save_to_file (fixture->key_file, fixture->tmp_path, &error);
  g_assert_no_error (error);
}

static void
setup (Fixture      *fixture,
       gconstpointer unused)
{
  GFileIOStream *stream;
  fixture->tmp_file = g_file_new_tmp (TESTING_FILE_PATH, &stream, NULL);
  fixture->tmp_path = g_file_get_path (fixture->tmp_file);

  fixture->key_file = g_key_file_new ();
  write_testing_keyfile (fixture, STARTING_KEY_FILE);

  fixture->network_send_provider =
    emer_network_send_provider_new_full (fixture->tmp_path);
}

static void
teardown (Fixture      *fixture,
          gconstpointer unused)
{
  g_key_file_unref (fixture->key_file);
  g_object_unref (fixture->tmp_file);
  g_unlink (fixture->tmp_path);
  g_free (fixture->tmp_path);
  g_object_unref (fixture->network_send_provider);
}

static void
test_network_send_provider_new_succeeds (Fixture      *fixture,
                                         gconstpointer unused)
{
  g_assert (fixture->network_send_provider != NULL);
}

static void
test_network_send_provider_can_get_send_number (Fixture      *fixture,
                                                gconstpointer unused)
{
  gint send_number;
  g_assert (emer_network_send_provider_get_send_number (fixture->network_send_provider,
                                                        &send_number));
  g_assert_cmpint (send_number, ==, STARTING_SEND_NUMBER);
}

static void
test_network_send_provider_caches_send_number (Fixture      *fixture,
                                               gconstpointer unused)
{
  // First read should cache the value.
  gint first_send_number;
  g_assert (emer_network_send_provider_get_send_number (fixture->network_send_provider,
                                                        &first_send_number));
  g_assert_cmpint (first_send_number, ==, STARTING_SEND_NUMBER);

  // This key_file should now be ignored by the provider.
  write_testing_keyfile (fixture, OTHER_KEY_FILE);

  gint second_send_number;
  g_assert (emer_network_send_provider_get_send_number (fixture->network_send_provider,
                                                        &second_send_number));

  // Should not have changed.
  g_assert_cmpint (second_send_number, ==, STARTING_SEND_NUMBER);
}

static void
test_network_send_provider_can_increment_send_number (Fixture      *fixture,
                                                      gconstpointer unused)
{
  g_assert (emer_network_send_provider_increment_send_number (fixture->network_send_provider));

  gint incremented_send_number;
  g_assert (emer_network_send_provider_get_send_number (fixture->network_send_provider,
                                                        &incremented_send_number));
  g_assert_cmpint (incremented_send_number, ==, STARTING_SEND_NUMBER + 1);
}

static void
test_network_send_provider_resets_when_corrupted (Fixture      *fixture,
                                                  gconstpointer unused)
{
  write_testing_keyfile (fixture, INVALID_KEY_FILE);

  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "*Failed to read from network send file. "
                         "Resetting data.*");

  // Value will never be mutated by the failing function.
  gint starting_invalid_send_number = -1;
  gint invalid_send_number = starting_invalid_send_number;

  // Reading from an invalid file should reset the key file and throw a warning.
  g_assert_false (emer_network_send_provider_get_send_number (fixture->network_send_provider,
                                                              &invalid_send_number));

  g_test_assert_expected_messages ();
  g_assert_cmpint (invalid_send_number, ==, starting_invalid_send_number);

  /* Setting this to a non-0 value to prevent a false positive. The reset values
     are 0, so this will avoid that. */
  gint reset_send_number = -1;

  g_assert (emer_network_send_provider_get_send_number (fixture->network_send_provider,
                                                        &reset_send_number));
  g_assert_cmpint (reset_send_number, ==, RESET_SEND_NUMBER);
}

int
main (int                argc,
      const char * const argv[])
{
  g_test_init (&argc, (char ***) &argv, NULL);
#define ADD_CACHE_VERSION_TEST_FUNC(path, func) \
  g_test_add ((path), Fixture, NULL, setup, (func), teardown)

  ADD_CACHE_VERSION_TEST_FUNC ("/network-send-provider/new-succeeds",
                               test_network_send_provider_new_succeeds);
  ADD_CACHE_VERSION_TEST_FUNC ("/network-send-provider/can-get-send-number",
                               test_network_send_provider_can_get_send_number);
  ADD_CACHE_VERSION_TEST_FUNC ("/network-send-provider/caches-send-number",
                               test_network_send_provider_caches_send_number);
  ADD_CACHE_VERSION_TEST_FUNC ("/network-send-provider/can-increment-send-number",
                               test_network_send_provider_can_increment_send_number);
  ADD_CACHE_VERSION_TEST_FUNC ("/network-send-provider/resets-when-corrupted",
                               test_network_send_provider_resets_when_corrupted);

#undef ADD_CACHE_VERSION_TEST_FUNC

  return g_test_run ();
}
