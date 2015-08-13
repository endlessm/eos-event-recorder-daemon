#!/usr/bin/env python3

# Copyright 2014, 2015 Endless Mobile, Inc.

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

import dbus
import shlex
import shutil
import subprocess
import tempfile
import unittest

import dbusmock

_METRICS_IFACE = 'com.endlessm.Metrics.EventRecorderServer'


class TestOptOutIntegration(dbusmock.DBusTestCase):
    """
    Makes sure the Enabled property can be set and retrieved.

    FIXME This test could be made more useful by adding an option to run the
    daemon with a different config file, which could then be written to in a
    unit test.
    """
    @classmethod
    def setUpClass(klass):
        """Set up a mock system bus."""
        klass.start_system_bus()
        klass.dbus_con = klass.get_dbus(system_bus=True)

    def setUp(self):
        """Start the event recorder on the mock system bus."""

        # Put polkitd and logind mocks onto the mock system bus
        (self.login_popen, self.login_obj) = self.spawn_server_template('logind')
        (self.polkit_popen, self.polkit_obj) = self.spawn_server_template('polkitd')

        self.persistent_cache_directory = tempfile.mkdtemp()
        escaped_dir = shlex.quote(self.persistent_cache_directory)
        persistent_cache_dir_arg = '--persistent-cache-directory=' + escaped_dir
        self.daemon = subprocess.Popen(['./eos-metrics-event-recorder',
                                        persistent_cache_dir_arg])

        # Wait for the service to come up
        self.wait_for_bus_object('com.endlessm.Metrics',
            '/com/endlessm/Metrics', system_bus=True)

        metrics_object = self.dbus_con.get_object('com.endlessm.Metrics',
            '/com/endlessm/Metrics')
        self.interface = dbus.Interface(metrics_object, _METRICS_IFACE)

    def tearDown(self):
        self.polkit_popen.terminate()
        self.login_popen.terminate()
        self.daemon.terminate()

        self.polkit_popen.wait()
        self.login_popen.wait()
        self.assertEquals(self.daemon.wait(), 0)
        shutil.rmtree(self.persistent_cache_directory)

    def test_opt_out_readable(self):
        """Make sure the Enabled property exists."""
        self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE)

    def test_opt_out_not_writable(self):
        """Make sure the Enabled property is not writable."""
        with self.assertRaisesRegexp(dbus.DBusException, 'org\.freedesktop\.DBus\.Error\.InvalidArgs'):
            self.interface.Set(_METRICS_IFACE, 'Enabled', False,
                dbus_interface=dbus.PROPERTIES_IFACE)

    def test_set_enabled_authorized(self):
        """
        Make sure the Enabled property's value persists and accessing SetEnabled
        succeeds when it is set to allowed.
        """
        self.polkit_obj.SetAllowed(['com.endlessm.Metrics.SetEnabled'])
        self.interface.SetEnabled(True)
        self.assertTrue(self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE))
        self.interface.SetEnabled(False)
        self.assertFalse(self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE))

    def test_set_enabled_unauthorized(self):
        """
        Make sure that accessing SetEnabled fails if not explicitly authorized.
        """
        with self.assertRaisesRegexp(dbus.DBusException, 'org\.freedesktop\.DBus\.Error\.AuthFailed'):
            self.interface.SetEnabled(True)


if __name__ == '__main__':
    unittest.main()
