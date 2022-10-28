#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define MOCK_TYPE_NETWORK_MONITOR (mock_network_monitor_get_type())

G_DECLARE_FINAL_TYPE (MockNetworkMonitor, mock_network_monitor, MOCK, NETWORK_MONITOR, GObject)

MockNetworkMonitor *mock_network_monitor_new (void);

G_END_DECLS
