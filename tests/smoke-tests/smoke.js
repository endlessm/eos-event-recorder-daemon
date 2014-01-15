const EosMetrics = imports.gi.EosMetrics;
const GLib = imports.gi.GLib;
const Lang = imports.lang;
const Mainloop = imports.mainloop;

// Before you run this smoke test, make sure you are not posting metrics data to
// a live metrics server!!

// Custom sender class
const SmokeGrinderMetricsSender = new Lang.Class({
    Name: 'SmokeGrinderMetricsSender',
    Extends: EosMetrics.Sender,

    URI_CONTEXT: 'ground_smoke',
    FORM_PARAM_NAME: 'report',
    QUEUE_FILE_NAME: 'smoke_queue.json',

    _init: function (props) {
        props = props || {};
        let storage_dir = EosMetrics.get_default_storage_dir();
        props.storage_file = storage_dir.get_child(this.QUEUE_FILE_NAME);
        props.connection = new EosMetrics.Connection({
            form_param_name: this.FORM_PARAM_NAME,
            uri_context: this.URI_CONTEXT
        });
        this.parent(props);
    }
});

// Convenience function for creating the data payloads that this app sends
function create_payload(units, time) {
    return new GLib.Variant('a{sv}', {
        units_of_smoke_ground: new GLib.Variant('u', units),
        grinding_time: new GLib.Variant('u', time)
    });
}

// At application startup
let sender = new SmokeGrinderMetricsSender();

// When posting metrics data, e.g. reporting that 5 units of smoke were ground
// in 300 seconds
let payload = create_payload(5, 300);
sender.send_data(payload, null, function (obj, res) {
    sender.send_data_finish(res);
    Mainloop.quit();
});
// Return control to the main loop, the data will be sent or queued without
// blocking your GUI
Mainloop.run();
