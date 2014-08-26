// Copyright 2014 Endless Mobile, Inc.

imports.searchPath.unshift('.');

const GLib = imports.gi.GLib;
const EosMetrics = imports.gi.EosMetrics;
const SmokeLibrary = imports.smokeLibrary;

// Change this value to adjust size of GVariant payload.
const SIZE = 300;

function make_payload () {
    let make_big64 = function () {
        return new GLib.Variant('x', SmokeLibrary.randint(80000));
    };
    let str = '';
    let variant_data = {};
    for (let i = 0; i < SIZE; i++) {
        str = SmokeLibrary.make_next_string(str);
        variant_data[str] = make_big64();
    }
    return new GLib.Variant('a{sv}', variant_data);
}

SmokeLibrary.record_many_events(EosMetrics.EventRecorder.get_default(), 20,
                                make_payload);
