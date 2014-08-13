// Copyright 2014 Endless Mobile, Inc.

imports.searchPath.unshift('.');

const Lang = imports.lang;
const GLib = imports.gi.GLib;
const Gio = imports.gi.Gio;
const SmokeLibrary = imports.smokeLibrary;

const TEST_APPLICATION_ID = 'com.endlessm.example.smoketest';

const TestApplication = new Lang.Class ({
    Name: 'TestApplication',
    Extends: Gio.Application,

    vfunc_startup: function() {
        this.parent();

        GLib.timeout_add(GLib.PRIORITY_HIGH, 500, function () {
            this.quit();
        }.bind(this));
    },
    vfunc_activate: function() {}
});

function run_application () {
    if (!SmokeLibrary.environment_is_permitted())
        return;

    let app = new TestApplication({ application_id: TEST_APPLICATION_ID,
                                    flags: 0 });
    app.run(ARGV);
}

run_application();
