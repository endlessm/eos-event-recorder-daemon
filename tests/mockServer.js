const GLib = imports.gi.GLib;
const GObject = imports.gi.GObject;
const Lang = imports.lang;
const Soup = imports.gi.Soup;

// This is a mock metrics server, mostly the same as the impromptu server in
// tests/test-web.c - except there are no threads in GJS, so this runs in the
// default main context.

const _MOCK_SERVER_PORT = 8123;

const MockServer = new Lang.Class({
    Name: 'MockServer',
    Extends: Soup.Server,
    Properties: {
        'behavior': GObject.ParamSpec.int('behavior', 'Behavior', '',
            GObject.ParamFlags.READABLE | GObject.ParamFlags.WRITABLE | GObject.ParamFlags.CONSTRUCT_ONLY,
            0, 2, 0)
    },

    EXPECTED_USERNAME: 'endlessos',
    EXPECTED_PASSWORD: 'sosseldne',
    URI_CONTEXT: 'context',

    _init: function (props) {
        props = props || {};
        props.port = _MOCK_SERVER_PORT;

        this._handlerCalledCount = 0;
        this._behavior = null;
        this.messagesReceived = [];

        this.parent(props);

        let authDomain = new Soup.AuthDomainBasic({
            realm: 'Test Realm',
            add_path: '/' + this.URI_CONTEXT
        });
        authDomain.basic_set_auth_callback(Lang.bind(this,
            this._authorizeCallback));
        this.add_auth_domain(authDomain);

        switch(this.behavior) {
        case MockServer.BehaviorType.ALWAYS_REJECT:
            this.add_handler('/' + this.URI_CONTEXT, Lang.bind(this,
                this._rejectHandler));
            break;
        case MockServer.BehaviorType.SOMETIMES_REJECT:
            this.add_handler('/' + this.URI_CONTEXT, Lang.bind(this,
                this._sometimesRejectHandler));
            break;
        default:
            this.add_handler('/' + this.URI_CONTEXT, Lang.bind(this,
                this._acceptHandler));
        }
    },

    _acceptHandler: function (server, message, path, query, client) {
        let data = JSON.parse(message.request_body.flatten().get_data());
        this.messagesReceived.push(data);
        message.set_status(200);
    },

    _rejectHandler: function (server, message, path, query, client) {
        message.set_status(404);
    },

    _sometimesRejectHandler: function (server, message, path, query, client) {
        this._handlerCalledCount++;
        if (this._handlerCalledCount % 4 === 0 ||
            this._handlerCalledCount % 4 === 1)
            this._acceptHandler(server, message, path, query, client);
        else
            this._rejectHandler(server, message, path, query, client);
    },

    _authorizeCallback: function (domain, message, user, pass) {
        return (user === this.EXPECTED_USERNAME &&
            pass === this.EXPECTED_PASSWORD);
    },

    get behavior() {
        return this._behavior;
    },

    set behavior(value) {
        this._behavior = value;
    }
});

MockServer.BehaviorType = {
    ALWAYS_ACCEPT: 0,
    ALWAYS_REJECT: 1,
    SOMETIMES_REJECT: 2
};
