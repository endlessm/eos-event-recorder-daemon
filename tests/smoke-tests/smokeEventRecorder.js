// Copyright 2014 Endless Mobile, Inc.

imports.searchPath.unshift('.');

const GLib = imports.gi.GLib;
const EosMetrics = imports.gi.EosMetrics;
const SmokeLibrary = imports.smokeLibrary;

function make_payload (flavor) {
    switch (flavor) {
        case 'cows':
            return new GLib.Variant('a{sv}', {
                    cows: new GLib.Variant('u', SmokeLibrary.randint(10)),
                    pounds_of_grass: new GLib.Variant('d', SmokeLibrary.randfloat(7.5)),
                    hungry: new GLib.Variant('b', SmokeLibrary.randbool())
                });
        case 'sand':
            return new GLib.Variant('a{sv}', {
                    grains_of_sand: new GLib.Variant('t', SmokeLibrary.randint(1543)),
                    secret_message: new GLib.Variant('s', 'The Sand-Cake is a Lie!')
                });
        case 'moody horse':
            let possible_moods = ['Happy', 'Nonplussed', 'Moody', 'Angry'];
            return new GLib.Variant('s', possible_moods[SmokeLibrary.randint(4)]);
        default:
            return null;
    }
}

SmokeLibrary.record_many_events(EosMetrics.EventRecorder.get_default(), 20,
                                make_payload);
