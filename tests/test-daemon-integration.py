import os
import dbus
import subprocess
import unittest
import time
import uuid

import dbusmock

from gi.repository import EosMetrics
from gi.repository.GLib import Variant



class TestDaemonIntegration(dbusmock.DBusTestCase):
    _MOCK_EVENT_NOTHING_HAPPENED = '5071dd96-bdad-4ee5-9c26-3dfef34a9963'
    _MOCK_EVENT_NOTHING_HAPPENED_BYTES = uuid.UUID(_MOCK_EVENT_NOTHING_HAPPENED).bytes
    _NANOSECONDS_PER_SECOND = 1000000000L
    """
    Makes sure that the app-facing EosMetrics.EventRecorder interface calls
    the com.endlessm.Metrics.EventRecorder DBus interface and marshals all its
    arguments properly.
    """
    @classmethod
    def setUpClass(klass):
        """Set up a mock system bus."""
        klass.start_system_bus()
        klass.dbus_con = klass.get_dbus(system_bus=True)

    def setUp(self):
        self.dbus_mock = self.spawn_server('com.endlessm.Metrics',
            '/com/endlessm/Metrics', 'com.endlessm.Metrics.EventRecorderServer',
            system_bus=True, stdout=subprocess.PIPE)

        self.interface_mock = dbus.Interface(self.dbus_con.get_object(
            'com.endlessm.Metrics', '/com/endlessm/Metrics'),
            dbusmock.MOCK_IFACE)

        self.interface_mock.AddMethod('', 'RecordSingularEvent', 'uayxbv', '', '')
        self.interface_mock.AddMethod('', 'RecordAggregateEvent', 'uayxxbv', '', '')
        self.interface_mock.AddMethod('', 'RecordEventSequence', 'uaya(xbv)', '', '')

        self.event_recorder = EosMetrics.EventRecorder()
        self.interface_mock.ClearCalls()

    def tearDown(self):
        self.dbus_mock.terminate()
        self.dbus_mock.wait()

    def call_singular_event(self, payload=None):
        self.event_recorder.record_event(self._MOCK_EVENT_NOTHING_HAPPENED, payload)
        return self.interface_mock.GetCalls()

    def call_aggregate_event(self, num_events=2, payload=None):
        self.event_recorder.record_events(self._MOCK_EVENT_NOTHING_HAPPENED, num_events, payload)
        return self.interface_mock.GetCalls()

    def call_start_stop_event(self, payload_start=None, payload_stop=None):
        self.event_recorder.record_start(self._MOCK_EVENT_NOTHING_HAPPENED, None, payload_start)
        self.event_recorder.record_stop(self._MOCK_EVENT_NOTHING_HAPPENED, None, payload_stop)
        return self.interface_mock.GetCalls()

    ### Recorder calls D-BUS at all. ###
    def test_record_singular_event_calls_dbus(self):
        calls = self.call_singular_event()
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1], 'RecordSingularEvent')

    def test_record_aggregate_event_calls_dbus(self):
        calls = self.call_aggregate_event()
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0][1], 'RecordAggregateEvent')

    def test_record_event_sequence_calls_dbus(self):
        calls = self.call_start_stop_event()
        self.assertEqual(len(calls), 1) # Dbus is only called from "stop".
        self.assertEqual(calls[0][1], 'RecordEventSequence')


    ### User Id isn't garbled. ###
    def test_record_singular_event_passes_uid(self):
        calls = self.call_singular_event()
        self.assertEqual(calls[0][2][0], os.getuid())

    def test_record_aggregate_event_passes_uid(self):
        calls = self.call_aggregate_event()
        self.assertEqual(calls[0][2][0], os.getuid())

    def test_record_event_sequence_passes_uid(self):
        calls = self.call_start_stop_event()
        self.assertEqual(calls[0][2][0], os.getuid())


    ### Event Id is't garbled. ###
    def test_record_singular_event_passes_event_id(self):
        calls = self.call_singular_event()
        actual_bytes = "".join(map(chr, calls[0][2][1]))
        self.assertEqual(self._MOCK_EVENT_NOTHING_HAPPENED_BYTES, actual_bytes)

    def test_record_aggregate_event_passes_event_id(self):
        calls = self.call_aggregate_event()
        actual_bytes = "".join(map(chr, calls[0][2][1]))
        self.assertEqual(self._MOCK_EVENT_NOTHING_HAPPENED_BYTES, actual_bytes)

    def test_record_event_sequence_passes_event_id(self):
        calls = self.call_start_stop_event()
        actual_bytes = "".join(map(chr, calls[0][2][1]))
        self.assertEqual(self._MOCK_EVENT_NOTHING_HAPPENED_BYTES, actual_bytes)


    ### Aggregated events' count isn't garbled. ###
    def test_record_aggregate_event_passes_event_count(self):
        leet_count = 1337
        calls = self.call_aggregate_event(num_events=leet_count)
        self.assertEqual(calls[0][2][2], leet_count)


    ### Timestamps are monotonically increasing. ###
    def test_record_singular_event_has_increasing_relative_timestamp(self):
        calls = self.call_singular_event()
        first_time = calls[0][2][2]
        self.interface_mock.ClearCalls()
        calls = self.call_singular_event()
        second_time = calls[0][2][2]
        self.assertLessEqual(first_time, second_time)

    def test_record_aggregate_event_has_increasing_relative_timestamp(self):
        calls = self.call_aggregate_event()
        first_time = calls[0][2][3]
        self.interface_mock.ClearCalls()
        self.call_aggregate_event()
        calls = self.interface_mock.GetCalls()
        second_time = calls[0][2][3]
        self.assertLessEqual(first_time, second_time)

    def test_record_event_sequence_has_increasing_relative_timestamp(self):
        calls = self.call_start_stop_event()
        first_time = calls[0][2][2][0][0]
        second_time = calls[0][2][2][1][0]
        self.interface_mock.ClearCalls()
        calls = self.call_start_stop_event()
        third_time = calls[0][2][2][0][0]
        fourth_time = calls[0][2][2][1][0]
        self.assertLessEqual(first_time, second_time)
        self.assertLessEqual(second_time, third_time)
        self.assertLessEqual(third_time, fourth_time)


    ### The difference between two relative timestamps is non-negative and
    ### less than that of two surrounding absolute timestamps.
    def test_record_singular_event_has_reasonable_relative_timestamp(self):
        absolute_time_first = time.time()

        calls = self.call_singular_event()
        relative_time_first = calls[0][2][2]
        self.interface_mock.ClearCalls()
        calls = self.call_singular_event()
        relative_time_second = calls[0][2][2]

        absolute_time_second = time.time()

        relative_time_difference = relative_time_second - relative_time_first
        self.assertLessEqual(0, relative_time_difference)
        absolute_time_difference = (absolute_time_second - absolute_time_first) * self._NANOSECONDS_PER_SECOND
        self.assertLessEqual(relative_time_difference, absolute_time_difference)

    def test_record_aggregate_event_has_reasonable_relative_timestamp(self):
        absolute_time_first = time.time()

        calls = self.call_aggregate_event()
        relative_time_first = calls[0][2][3]
        self.interface_mock.ClearCalls()
        calls = self.call_aggregate_event()
        relative_time_second = calls[0][2][3]

        absolute_time_second = time.time()

        relative_time_difference = relative_time_second - relative_time_first
        self.assertLessEqual(0, relative_time_difference)
        absolute_time_difference = (absolute_time_second - absolute_time_first) * self._NANOSECONDS_PER_SECOND
        self.assertLessEqual(relative_time_difference, absolute_time_difference)

    def test_record_event_sequence_has_reasonable_relative_timestamp(self):
        absolute_time_first = time.time()

        calls = self.call_start_stop_event()
        relative_time_first = calls[0][2][2][0][0]

        self.interface_mock.ClearCalls()
        calls = self.call_start_stop_event()
        relative_time_second = calls[0][2][2][1][0]

        absolute_time_second = time.time()

        relative_time_difference = relative_time_second - relative_time_first
        self.assertLessEqual(0, relative_time_difference)
        absolute_time_difference = (absolute_time_second - absolute_time_first) * self._NANOSECONDS_PER_SECOND
        self.assertLessEqual(relative_time_difference, absolute_time_difference)


    ### The maybe type is emulated correctly for empty payloads. ###
    def test_record_singular_event_maybe_flag_is_false_when_payload_is_empty(self):
        calls = self.call_singular_event(payload=None)
        self.assertEqual(calls[0][2][3], False)

    def test_record_aggregate_event_maybe_flag_is_false_when_payload_is_empty(self):
        calls = self.call_aggregate_event(payload=None)
        self.assertEqual(calls[0][2][4], False)

    def test_record_event_sequence_maybe_flags_are_false_when_payloads_are_empty(self):
        self.event_recorder.record_start(self._MOCK_EVENT_NOTHING_HAPPENED, None, None)
        self.event_recorder.record_progress(self._MOCK_EVENT_NOTHING_HAPPENED, None, None)
        self.event_recorder.record_stop(self._MOCK_EVENT_NOTHING_HAPPENED, None, None)
        calls = self.interface_mock.GetCalls()
        self.assertEqual(calls[0][2][2][0][1], False)
        self.assertEqual(calls[0][2][2][1][1], False)
        self.assertEqual(calls[0][2][2][2][1], False)


   ### The maybe type is emulated correctly for non-empty payloads. ###
    def test_record_singluar_event_maybe_flag_is_true_when_payload_is_not_empty(self):
        # Contains both a Matt and not a Matt until viewed.
        calls = self.call_singular_event(payload=Variant.new_string("Quantum Dalio"))
        self.assertEqual(calls[0][2][3], True)

    def test_record_aggregate_event_maybe_flag_is_true_when_payload_is_not_empty(self):
        # Let's be serious, when is the last time *you* saw a fiscally responsible mime?
        calls = self.call_aggregate_event(payload=Variant.new_string("Fiscally Responsible Mime"))
        self.assertEqual(calls[0][2][4], True)

    def test_record_event_sequence_maybe_flag_is_true_when_payload_is_not_empty(self):
        self.event_recorder.record_start(self._MOCK_EVENT_NOTHING_HAPPENED, None, Variant.new_string("Flagrant n00b"))
        self.event_recorder.record_progress(self._MOCK_EVENT_NOTHING_HAPPENED, None, Variant.new_string("Murphy"))
        self.event_recorder.record_stop(self._MOCK_EVENT_NOTHING_HAPPENED, None, Variant.new_string("What's that blue thing, doing here?"))
        # +5 Bonus points if you know where that last one comes from.
        calls = self.interface_mock.GetCalls()
        self.assertEqual(calls[0][2][2][0][1], True)
        self.assertEqual(calls[0][2][2][1][1], True)
        self.assertEqual(calls[0][2][2][2][1], True)


    ### The payloads are not garbled. ###
    def test_record_singular_event_passes_payload(self):
        logic_string = "Occam's Razor"
        # It has gotten dull with use.
        calls = self.call_singular_event(payload=Variant.new_string("Occam's Razor"))
        self.assertEqual(calls[0][2][4], logic_string)

    def test_record_aggregate_event_passes_payload(self):
        rupert_string = "Prince Rupert's Drop"
        # If you haven't seen this, you need to.
        calls = self.call_aggregate_event(payload=Variant.new_string(rupert_string))
        self.assertEqual(calls[0][2][5], rupert_string)

    def test_record_event_sequence_passes_payloads(self):
        start_string = "I am a jelly donut(sic)."
        progress_string = "It's in a better place. Or rather, it's in the same place, but now its got a big hole through it!"
        end_string = "How dare you dodge the barrel!"
        self.event_recorder.record_start(self._MOCK_EVENT_NOTHING_HAPPENED, None, Variant.new_string(start_string))
        self.event_recorder.record_progress(self._MOCK_EVENT_NOTHING_HAPPENED, None, Variant.new_string(progress_string))
        self.event_recorder.record_stop(self._MOCK_EVENT_NOTHING_HAPPENED, None, Variant.new_string(end_string))
        # +1 Bonus point for the first, +5 for the second and +10 for the third reference.
        # I'm keeping track, don't you worry.
        calls = self.interface_mock.GetCalls()
        self.assertEqual(calls[0][2][2][0][2], start_string)
        self.assertEqual(calls[0][2][2][1][2], progress_string)
        self.assertEqual(calls[0][2][2][2][2], end_string)


if __name__ == '__main__':
    unittest.main()
