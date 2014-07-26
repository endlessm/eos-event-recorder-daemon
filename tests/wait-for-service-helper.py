import sys
from gi.repository import GLib, Gio


DBUS_NAME = 'com.endlessm.Metrics'


def on_name_appeared(connection, name, owner):
    print DBUS_NAME, 'appeared'
    Gio.bus_unwatch_name(watcher_id)
    loop.quit()


def on_timeout():
    print 'Timed out'
    Gio.bus_unwatch_name(watcher_id)
    sys.exit(1)

print 'Watching for name', DBUS_NAME
watcher_id = Gio.bus_watch_name(Gio.BusType.SYSTEM, DBUS_NAME,
    Gio.BusNameWatcherFlags.NONE, on_name_appeared, None)
GLib.timeout_add_seconds(5, on_timeout)

loop = GLib.MainLoop()
loop.run()
