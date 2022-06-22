#!/usr/bin/env python3

# Copyright 2014, 2015, 2017 Endless Mobile, Inc.

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

import configparser
import dbus
import os
import sqlite3
import subprocess
import taptestrunner
import tempfile
import time
import unittest
import uuid

import dbusmock

_METRICS_IFACE = 'com.endlessm.Metrics.EventRecorderServer'
_TIMER_IFACE = "com.endlessm.Metrics.AggregateTimer"


class TestOptOutIntegration(dbusmock.DBusTestCase):
    """
    Makes sure the Enabled property can be set and retrieved.
    """
    @classmethod
    def setUpClass(klass):
        """Set up a mock system bus."""
        klass.start_system_bus()
        klass.dbus_con = klass.get_dbus(system_bus=True)

    def setUp(self):
        """Start the event recorder on the mock system bus."""

        # Put polkitd mocks onto the mock system bus.
        (self.polkit_popen, self.polkit_obj) = self.spawn_server_template('polkitd')

        self.test_dir = tempfile.TemporaryDirectory(
            prefix='eos-event-recorder-daemon-test.')

        persistent_cache_directory = os.path.join(self.test_dir.name, 'cache')
        persistent_cache_dir_arg = '--persistent-cache-directory=' + persistent_cache_directory

        self.config_file = os.path.join(self.test_dir.name, 'permissions.conf')
        config_file_arg = '--config-file-path={}'.format(self.config_file)

        daemon_path = os.environ.get('EMER_PATH', './eos-metrics-event-recorder')
        self.daemon = subprocess.Popen([daemon_path,
                                        persistent_cache_dir_arg,
                                        config_file_arg])

        # Wait for the service to come up
        self.wait_for_bus_object('com.endlessm.Metrics',
            '/com/endlessm/Metrics', system_bus=True)

        metrics_object = self.dbus_con.get_object('com.endlessm.Metrics',
            '/com/endlessm/Metrics')
        self.interface = dbus.Interface(metrics_object, _METRICS_IFACE)

        self.db = sqlite3.connect(os.path.join(persistent_cache_directory, "metrics.db"))

    def tearDown(self):
        self.polkit_popen.terminate()
        self.daemon.terminate()

        self.polkit_popen.wait()
        self.assertEqual(self.daemon.wait(), 0)

        self.db.close()

        self.test_dir.cleanup()

    def test_opt_out_readable(self):
        """Make sure the Enabled property exists."""
        self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE)

    def test_opt_out_not_writable(self):
        """Make sure the Enabled property is not writable."""
        with self.assertRaisesRegex(dbus.DBusException, r'org\.freedesktop\.DBus\.Error\.InvalidArgs'):
            self.interface.Set(_METRICS_IFACE, 'Enabled', False,
                dbus_interface=dbus.PROPERTIES_IFACE)

    def test_set_enabled_authorized(self):
        """
        Make sure the Enabled property's value persists and accessing SetEnabled
        succeeds when it is set to allowed.
        """
        # Check defaults look good and erase the file before our next change
        self._check_config_file(enabled='true', uploading_enabled='false')

        self.polkit_obj.SetAllowed(['com.endlessm.Metrics.SetEnabled'])
        self.interface.SetEnabled(True)
        self.assertTrue(self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE))

        self._check_config_file(enabled='true', uploading_enabled='true')

        self.interface.SetEnabled(False)
        self.assertFalse(self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE))

        self._check_config_file(enabled='false', uploading_enabled='false')

    def test_set_enabled_unauthorized(self):
        """
        Make sure that accessing SetEnabled fails if not explicitly authorized.
        """
        with self.assertRaisesRegex(dbus.DBusException, r'org\.freedesktop\.DBus\.Error\.AuthFailed'):
            self.interface.SetEnabled(True)

    def test_upload_doesnt_change_config(self):
        """
        Make sure that calling UploadEvents() doesn't spontaneously enable
        uploading. This seems implausible but did actually happen.
        UploadEvents() causes the config to be re-read, triggering a change
        notification on EmerPermissionsProvider:enabled, triggering a (no-op)
        update of the Enabled D-Bus property to TRUE, which was bound to
        EmerPermissionsProvider:uploading-enabled so caused that property to
        be set to TRUE.
        """
        # Check defaults look good and erase the file before our next change
        self._check_config_file(enabled='true', uploading_enabled='false')

        with self.assertRaisesRegex(dbus.exceptions.DBusException,
                                    r'uploading is disabled') as context:
            self.interface.UploadEvents()
        self.assertEqual(context.exception.get_dbus_name(),
                         "com.endlessm.Metrics.Error.UploadingDisabled")

        self._check_config_file(enabled='true', uploading_enabled='false')

    def test_UploadEvents_fails_if_disabled(self):
        self.polkit_obj.SetAllowed(['com.endlessm.Metrics.SetEnabled'])
        self.interface.SetEnabled(False)
        with self.assertRaisesRegex(dbus.exceptions.DBusException,
                                    r'metrics system is disabled') as context:
            self.interface.UploadEvents()
        self.assertEqual(context.exception.get_dbus_name(),
                         "com.endlessm.Metrics.Error.MetricsDisabled")

    def test_clears_tally_when_disabled(self):
        self.polkit_obj.SetAllowed(['com.endlessm.Metrics.SetEnabled'])
        self.interface.SetEnabled(True)
        event_id = uuid.UUID("350ac4ff-3026-4c25-9e7e-e8103b4fd5d8")
        monthly_event_id = uuid.uuid5(event_id, "monthly")
        timer_path = self.interface.StartAggregateTimer(
            0,
            event_id.bytes,
            False,
            False,
        )
        timer = self.dbus_con.get_object('com.endlessm.Metrics', timer_path)
        timer.StopTimer(dbus_interface=_TIMER_IFACE)

        rows = self.db.execute("select event_id from tally order by event_id asc").fetchall()
        self.assertEqual(rows, sorted([(event_id.bytes,), (monthly_event_id.bytes,)]))

        self.interface.SetEnabled(False)
        rows = self.db.execute("select event_id from tally").fetchall()
        self.assertEqual(rows, [])

    def test_cancels_running_timer_when_disabled(self):
        self.polkit_obj.SetAllowed(['com.endlessm.Metrics.SetEnabled'])
        self.interface.SetEnabled(True)
        event_id = uuid.UUID("350ac4ff-3026-4c25-9e7e-e8103b4fd5d8")
        timer_path = self.interface.StartAggregateTimer(
            0,
            event_id.bytes,
            False,
            False,
        )
        timer = self.dbus_con.get_object('com.endlessm.Metrics', timer_path)

        self.interface.SetEnabled(False)
        with self.assertRaises(dbus.exceptions.DBusException) as context:
            timer.StopTimer(dbus_interface=_TIMER_IFACE)

        self.assertEqual(context.exception.get_dbus_name(),
                         "org.freedesktop.DBus.Error.UnknownMethod")

        rows = self.db.execute("select event_id from tally").fetchall()
        self.assertEqual(rows, [])

    def test_StartAggregateEvent_fails_if_disabled(self):
        self.polkit_obj.SetAllowed(['com.endlessm.Metrics.SetEnabled'])
        self.interface.SetEnabled(False)
        with self.assertRaisesRegex(dbus.exceptions.DBusException,
                                    r'metrics system is disabled') as context:
            self.interface.StartAggregateTimer(
                0,
                uuid.UUID("350ac4ff-3026-4c25-9e7e-e8103b4fd5d8").bytes,
                False,
                False,
            ),
        self.assertEqual(context.exception.get_dbus_name(),
                         "com.endlessm.Metrics.Error.MetricsDisabled")

    def _check_config_file(self, enabled, uploading_enabled):
        # the config file is written asynchronously by the daemon,
        # so may not exist immediately after a change is made - wait
        # for up to 1 second for it to be written
        for i in range(20):
            if os.path.exists(self.config_file):
                break
            else:
                time.sleep(0.05)

        config = configparser.ConfigParser()
        self.assertEqual(config.read(self.config_file), [self.config_file])
        self.assertEqual(config.get("global", "enabled"), enabled)
        self.assertEqual(config.get("global", "uploading_enabled"), uploading_enabled)

        # erase the file after reading it to guarantee that the next time it
        # exists, it's up to date. the daemon doesn't read it once started.
        os.unlink(self.config_file)


if __name__ == '__main__':
    unittest.main(testRunner=taptestrunner.TAPTestRunner())
