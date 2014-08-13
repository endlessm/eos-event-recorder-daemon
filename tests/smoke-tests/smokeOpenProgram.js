// Copyright 2014 Endless Mobile, Inc.

imports.searchPath.unshift('.');

const GLib = imports.gi.GLib;
const SmokeLibrary = imports.smokeLibrary;

function stress_test_program_is_open (total_test_time) {
    if (!SmokeLibrary.environment_is_permitted())
        return;

    if (total_test_time <= 0)
        throw 'Non-positive test time given; cannot run program is open stress test.';
    for (let i = 0; i < total_test_time; i++) {
        GLib.spawn_async(null, ['gjs', 'smokeProgram.js'], null,
                         GLib.SpawnFlags.SEARCH_PATH, null);
        GLib.usleep(1000000); // 1,000,000 microseconds = 1 sec.
    }
}

// Parameter: How long to run test in seconds.
//   A value of 43,200 seconds is 12 hours which might be useful for long-running
//   tests for memory leaks.
stress_test_program_is_open(8);
