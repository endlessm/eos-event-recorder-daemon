#!/bin/bash -e

function finally() {
    kill $DBUS_SESSION_BUS_PID
}

# This brings up a new session bus and pretends that it is the system bus.
# dbus-launch initializes DBUS_SESSION_BUS_PID.
eval `dbus-launch`
export DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS
export DBUS_SESSION_BUS_PID

# Take down the mock DBus, no matter whether we exit successfully or
# fail the tests; think of a bash trap as a "finally" clause.
trap finally EXIT

"$@" -k --tap
