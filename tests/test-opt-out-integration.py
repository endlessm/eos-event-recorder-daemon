import dbus
import subprocess
import time
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
        self.daemon = subprocess.Popen('./eos-metrics-event-recorder')

        # FIXME find a better way to wait for the service to come up
        time.sleep(1)

        metrics_object = self.dbus_con.get_object('com.endlessm.Metrics',
            '/com/endlessm/Metrics')
        self.interface = dbus.Interface(metrics_object, _METRICS_IFACE)

    def tearDown(self):
        self.daemon.terminate()

    def test_opt_out_readable(self):
        """Make sure the Enabled property exists."""
        self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE)

    def test_opt_out_writable(self):
        """
        Make sure the Enabled property is writable and its value persists.
        Currently this will cause the event recorder process to print an error
        because it can't write to the config file in /etc because it's not
        running as root; but that's OK.
        """
        self.interface.Set(_METRICS_IFACE, 'Enabled', True,
            dbus_interface=dbus.PROPERTIES_IFACE)
        self.assertTrue(self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE))
        self.interface.Set(_METRICS_IFACE, 'Enabled', False,
            dbus_interface=dbus.PROPERTIES_IFACE)
        self.assertFalse(self.interface.Get(_METRICS_IFACE, 'Enabled',
            dbus_interface=dbus.PROPERTIES_IFACE))


if __name__ == '__main__':
    unittest.main()
