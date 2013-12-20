/* Copyright 2013 Endless Mobile, Inc. */

/* global jasmine,describe,xdescribe,it,xit,beforeEach,afterEach,expect */

const EosMetrics = imports.gi.EosMetrics;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const Mainloop = imports.mainloop;

const MockServer = imports.mockServer.MockServer;

const MOCK_SERVER_PORT = 8123;
const MOCK_ENDPOINT = 'http://localhost:' + MOCK_SERVER_PORT;
const MAX_SEC_WAIT = 5;

describe('Testing the whole metrics kit against a mock server', function () {
    let endpointFile, tmpFile, fingerprintFile, storageFile, sender, server;
    let payloadData;

    beforeEach(function () {
        // Write an appropriate endpoint config file
        let [file, stream] = Gio.File.new_tmp('metricsintegrationtestendpointXXXXXX');
        endpointFile = file;
        stream.output_stream.write(JSON.stringify({ endpoint: MOCK_ENDPOINT }),
            null);
        stream.close(null);

        // Set up other paths
        let tmpPath = GLib.Dir.make_tmp('metricsintegrationtestXXXXXX');
        tmpFile = Gio.File.new_for_path(tmpPath);

        fingerprintFile = tmpFile.get_child('fingerprint');
        storageFile = tmpFile.get_child('storage.json');

        // Create the test object
        let connection = new EosMetrics.Connection({
            endpoint_config_file: endpointFile,
            fingerprint_file: fingerprintFile,
            uri_context: 'context',
            form_param_name: 'formParam'
        });
        sender = new EosMetrics.Sender({
            connection: connection,
            storage_file: storageFile
        });

        // Sanity check
        expect(sender.connection.endpoint).toBe(MOCK_ENDPOINT);

        // Dummy strings to put in metrics data
        payloadData = ['foo', 'bar', 'biz', 'baz'];
    });

    afterEach(function () {
        endpointFile.delete(null);
        fingerprintFile.delete(null);
        try {
            storageFile.delete(null);
        } catch(e if e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND)) {
            // pass
        }
        tmpFile.delete(null);
    });

    it('sends all data to the server', function () {
        server = new MockServer({
            behavior: MockServer.BehaviorType.ALWAYS_ACCEPT
        });

        _sendPayloadsThenQuit(2);
        printerr('running server async');
        server.run_async();
        printerr('running main loop');
        Mainloop.run();

        let data = server.messagesReceived;
        expect(data).toEqual([{
            formParam: {
                test: 'foo',
                fingerprint: jasmine.any(String),
                machine: jasmine.any(Number)
            }
        },
        {
            formParam: {
                test: 'bar',
                fingerprint: jasmine.any(String),
                machine: jasmine.any(Number)
            }
        }]);
        expect(data[0].formParam.fingerprint)
            .toEqual(data[1].formParam.fingerprint);
        expect(data[0].formParam.machine).toEqual(data[1].formParam.machine);
    });

    it('queues all data when the server is down', function () {
        server = new MockServer({
            behavior: MockServer.BehaviorType.ALWAYS_REJECT
        });

        _sendPayloadsThenQuit(2);
        server.run_async();
        Mainloop.run();

        expect(server.messagesReceived).toEqual([]);
        expect(_getQueuedData()).toEqual([
            { test: 'foo' },
            { test: 'bar' }
        ]);
    });

    it('queues some data when the server is unreliable', function () {
        server = new MockServer({
            behavior: MockServer.BehaviorType.SOMETIMES_REJECT
        });

        _sendPayloadsThenQuit(4);
        server.run_async();
        Mainloop.run();

        let data = server.messagesReceived;
        expect(data).toEqual([{
            formParam: {
                test: 'foo',
                fingerprint: jasmine.any(String),
                machine: jasmine.any(Number)
            }
        },
        {
            formParam: {
                test: 'baz',
                fingerprint: jasmine.any(String),
                machine: jasmine.any(Number)
            }
        }]);
        expect(data[0].formParam.fingerprint)
            .toEqual(data[1].formParam.fingerprint);
        expect(data[0].formParam.machine)
            .toEqual(data[1].formParam.machine);

        expect(_getQueuedData()).toEqual([
            { test: 'bar' },
            { test: 'biz' }
        ]);
    });

    // hurray for avoiding callback hell
    function _sendPayloadsThenQuit(num) {
        printerr('send then quit');
        let payload = new GLib.Variant('a{sv}', {
            test: new GLib.Variant('s', payloadData[0])
        });
        payloadData.push(payloadData.shift());

        sender.send_data(payload, null, function (obj, res) {
            printerr('callback');
            if (sender.send_data_finish(res) && num > 1)
                _sendPayloadsThenQuit(num - 1);
            else {
                server.disconnect();
                Mainloop.quit();
            }
        });

        printerr('installing timeout');
        // Abort the test if it's taking too long
        GLib.timeout_add_seconds(GLib.PRIORITY_HIGH, MAX_SEC_WAIT, function () {
            server.disconnect();
            Mainloop.quit();
        });
        printerr('done with function');
    }

    function _getQueuedData() {
        let [success, queuedData] = sender.storage_file.load_contents(null);
        return JSON.parse(queuedData);
    }
});
