#!/usr/bin/env python3

# Copyright 2022 Endless OS Foundation LLC

# This file is part of eos-event-recorder-daemon.
#
# eos-event-recorder-daemon is free software: you can redistribute it and/or
# modify it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or (at your
# option) any later version.
#
# eos-event-recorder-daemon is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
# Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with eos-event-recorder-daemon.  If not, see
# <http://www.gnu.org/licenses/>.

import datetime
import dbus
import os
import sqlite3
import subprocess
import taptestrunner
import tempfile
import unittest
import uuid

import dbusmock

from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

DBusGMainLoop(set_as_default=True)

_METRICS_IFACE = "com.endlessm.Metrics.EventRecorderServer"
_TIMER_IFACE = "com.endlessm.Metrics.AggregateTimer"


class TestRunningTimersOnShutdown(dbusmock.DBusTestCase):
    """
    Checks what happens to timers when the daemon is shut down.
    """

    @classmethod
    def setUpClass(klass):
        """Set up a mock system bus."""
        klass.start_system_bus()
        klass.dbus_con = klass.get_dbus(system_bus=True)

    def setUp(self):
        """Start the event recorder on the mock system bus."""

        # Put polkitd mocks onto the mock system bus.
        (self.polkit_popen, self.polkit_obj) = self.spawn_server_template("polkitd")

        self.test_dir = tempfile.TemporaryDirectory(
            prefix="eos-event-recorder-daemon-test."
        )

        persistent_cache_directory = os.path.join(self.test_dir.name, "cache")
        persistent_cache_dir_arg = (
            "--persistent-cache-directory=" + persistent_cache_directory
        )

        config_file = os.path.join(self.test_dir.name, "permissions.conf")
        config_file_arg = "--config-file-path={}".format(config_file)

        daemon_path = os.environ.get("EMER_PATH", "./eos-metrics-event-recorder")
        self.daemon = subprocess.Popen(
            [daemon_path, persistent_cache_dir_arg, config_file_arg]
        )

        # Wait for the service to come up
        self.wait_for_bus_object(
            "com.endlessm.Metrics", "/com/endlessm/Metrics", system_bus=True
        )

        self.metrics_object = self.dbus_con.get_object(
            "com.endlessm.Metrics", "/com/endlessm/Metrics"
        )
        self.interface = dbus.Interface(self.metrics_object, _METRICS_IFACE)

        self.db = sqlite3.connect(
            os.path.join(persistent_cache_directory, "metrics.db")
        )

    def tearDown(self):
        self.polkit_popen.terminate()
        self.daemon.terminate()

        self.polkit_popen.wait()
        self.assertEqual(self.daemon.wait(), 0)

        self.db.close()

        self.test_dir.cleanup()

    def test_timers_saved_on_clean_shutdown(self):
        """
        Tests that running timers are stored on a clean shutdown.
        """
        event_id = uuid.UUID("350ac4ff-3026-4c25-9e7e-e8103b4fd5d8")
        monthly_event_id = uuid.uuid5(event_id, "monthly")
        self.interface.StartAggregateTimer(
            0,
            event_id.bytes,
            False,
            False,
        )

        # FIXME: This test will fail when run across midnight.
        # It would be better to test from C (rather than over D-Bus) and
        # refactor the daemon to mock the passage of time.
        today = datetime.date.today()

        self.daemon.terminate()
        self.assertEqual(self.daemon.wait(), 0)

        rows = self.db.execute(
            "select event_id, date, counter from tally order by event_id asc"
        ).fetchall()
        event_ids, dates, counters = zip(*rows)
        self.assertEqual(
            [uuid.UUID(bytes=x) for x in event_ids],
            [event_id, monthly_event_id],
        )
        self.assertEqual(dates, (f"{today:%Y-%m-%d}", f"{today:%Y-%m}"))
        self.assertEqual(counters[0], counters[1])
        self.assertGreaterEqual(counters[0], 0)

    def test_timer_saved_after_client_disconnects(self):
        """
        If a client starts a timer, then disconnects from the bus without
        stopping that timer, the daemon should notice the client has gone
        away and save its running timer.
        """
        # Get a fresh connection to the (mock) system bus.
        bus = self.get_dbus(system_bus=True)
        self.assertIsNot(self.dbus_con, bus)

        # On the fresh connection, start a timer.
        metrics_object = bus.get_object("com.endlessm.Metrics", "/com/endlessm/Metrics")
        interface = dbus.Interface(metrics_object, _METRICS_IFACE)
        event_id = uuid.UUID("350ac4ff-3026-4c25-9e7e-e8103b4fd5d8")
        monthly_event_id = uuid.uuid5(event_id, "monthly")
        timer_path = interface.StartAggregateTimer(
            0,
            event_id.bytes,
            False,
            False,
        )

        # Close that connection, and wait to be told by the D-Bus daemon that
        # it has happened.
        loop = GLib.MainLoop()

        def name_owner_changed_cb(owner):
            loop.quit()

        watch = self.dbus_con.watch_name_owner(
            bus.get_unique_name(),
            name_owner_changed_cb,
        )
        bus.close()
        loop.run()

        # We know that the D-Bus daemon has sent the NameOwnerChanged signal
        # to all interested clients. In order to be sure that the daemon has
        # received & processed it, send a message to the daemon and wait for the
        # reply. When we receive the reply, we know that the daemon has
        # processed every earlier message from the bus, including
        # NameOwnerChanged.
        self.metrics_object.Get(
            _METRICS_IFACE, "Enabled", dbus_interface=dbus.PROPERTIES_IFACE
        )

        # By now we are sure that the daemon has received and processed the
        # NameOwnerChanged signal that notifies it that a client with a running
        # timer has disconnected. Check that it has saved its in-progress timer
        # to the database.
        rows = self.db.execute(
            "select event_id, date, counter from tally order by event_id asc"
        ).fetchall()
        event_ids, dates, counters = zip(*rows)
        self.assertEqual(
            [uuid.UUID(bytes=x) for x in event_ids],
            [event_id, monthly_event_id],
        )
        today = datetime.date.today()
        self.assertEqual(dates, (f"{today:%Y-%m-%d}", f"{today:%Y-%m}"))
        self.assertEqual(counters[0], counters[1])
        self.assertGreaterEqual(counters[0], 0)

        # And that the timer has been stopped.
        timer = self.dbus_con.get_object("com.endlessm.Metrics", timer_path)
        with self.assertRaises(dbus.exceptions.DBusException) as context:
            timer.StopTimer(dbus_interface=_TIMER_IFACE)

        self.assertEqual(
            context.exception.get_dbus_name(),
            "org.freedesktop.DBus.Error.UnknownMethod",
        )

    def test_two_overlapping_timers_from_same_client(self):
        """
        It can legitimately happen that the same client calls
        StartAggregateTimer() twice with the same arguments before stopping
        the first timer. The real-world example is gnome-shell having one timer
        per running app. Here's the sequence of events:

        - App A starts.
          - Shell starts a timer
            - Within the metrics client library, this begins an asynchronous
              call (1) to StartAggregateTimer()
        - App A quits.
          - Shell stops that timer (at the C API level)
            - Within the metrics client library, it can't call StopTimer() yet
              because (1) hasn't returned yet. So it sets a flag to call it
              when it can.
        - App A starts again.
          - Shell starts a timer
            - Within the metrics client library, this begins an asynchronous
              call (2) to StartAggregateTimer()

        Now the daemon will handle call 1, returning a path p, and handle call
        2, returning a path q. In turn, Shell will call p.StopTimer(). Some
        time later, app A will quit again, and Shell will call q.StopTimer().
        """
        event_id = uuid.UUID("350ac4ff-3026-4c25-9e7e-e8103b4fd5d8")
        monthly_event_id = uuid.uuid5(event_id, "monthly")
        p = self.interface.StartAggregateTimer(
            0,
            event_id.bytes,
            False,
            False,
        )
        q = self.interface.StartAggregateTimer(
            0,
            event_id.bytes,
            False,
            False,
        )

        # No assertion about whether p and q are different object paths -- this
        # is not guaranteed.

        self.dbus_con.get_object("com.endlessm.Metrics", p).StopTimer(dbus_interface=_TIMER_IFACE)
        # No assertion here about something having been stored in the database.
        # One possible implementation is to refcount identical timers from the
        # same client.
        self.dbus_con.get_object("com.endlessm.Metrics", q).StopTimer(dbus_interface=_TIMER_IFACE)

        rows = self.db.execute(
            "select event_id, date, counter from tally order by event_id asc"
        ).fetchall()
        event_ids, dates, counters = zip(*rows)
        self.assertEqual(
            [uuid.UUID(bytes=x) for x in event_ids],
            [event_id, monthly_event_id],
        )
        today = datetime.date.today()
        self.assertEqual(dates, (f"{today:%Y-%m-%d}", f"{today:%Y-%m}"))
        self.assertEqual(counters[0], counters[1])
        self.assertGreaterEqual(counters[0], 0)


if __name__ == "__main__":
    unittest.main(testRunner=taptestrunner.TAPTestRunner())
