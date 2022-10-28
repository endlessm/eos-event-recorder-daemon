#include "mock-network-monitor.h"

struct _MockNetworkMonitor
{
  GObject parent_instance;

  GNetworkConnectivity connectivity;
  gboolean network_available;
  gboolean network_metered;
};

static void initable_iface_init (GInitableIface *iface,
                                 gpointer        iface_data);
static void monitor_iface_init (GNetworkMonitorInterface *iface,
                                gpointer                  iface_data);

G_DEFINE_FINAL_TYPE_WITH_CODE (MockNetworkMonitor, mock_network_monitor, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init);
                               G_IMPLEMENT_INTERFACE (G_TYPE_NETWORK_MONITOR, monitor_iface_init);
                               )

enum {
  PROP_CONNECTIVITY = 1,
  PROP_NETWORK_AVAILABLE,
  PROP_NETWORK_METERED,
  N_PROPS
};

MockNetworkMonitor *
mock_network_monitor_new (void)
{
  return g_object_new (MOCK_TYPE_NETWORK_MONITOR, NULL);
}

#if 0
static void
mock_network_monitor_finalize (GObject *object)
{
  MockNetworkMonitor *self = (MockNetworkMonitor *)object;

  G_OBJECT_CLASS (mock_network_monitor_parent_class)->finalize (object);
}
#endif

static void
mock_network_monitor_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  MockNetworkMonitor *self = MOCK_NETWORK_MONITOR (object);

  switch (prop_id)
    {
    case PROP_CONNECTIVITY:
      g_value_set_enum (value, self->connectivity);
      break;

    case PROP_NETWORK_AVAILABLE:
      g_value_set_boolean (value, self->network_available);
      break;

    case PROP_NETWORK_METERED:
      g_value_set_boolean (value, self->network_metered);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mock_network_monitor_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  MockNetworkMonitor *self = MOCK_NETWORK_MONITOR (object);

  switch (prop_id)
    {
    case PROP_CONNECTIVITY:
      self->connectivity = g_value_get_enum (value);
      break;

    case PROP_NETWORK_AVAILABLE:
      self->network_available = g_value_get_boolean (value);
      break;

    case PROP_NETWORK_METERED:
      self->network_metered = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
mock_network_monitor_class_init (MockNetworkMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

#if 0
  object_class->finalize = mock_network_monitor_finalize;
#endif
  object_class->get_property = mock_network_monitor_get_property;
  object_class->set_property = mock_network_monitor_set_property;
}

static gboolean
mock_network_monitor_initable_init (GInitable    *initable,
                                    GCancellable *cancellable,
                                    GError      **error)
{
  return TRUE;
}


static void
initable_iface_init (GInitableIface *iface,
                     gpointer        iface_data)
{
  iface->init = mock_network_monitor_initable_init;
}

static void
monitor_iface_init (GNetworkMonitorInterface *iface,
                    gpointer                  iface_data)
{

}

static void
mock_network_monitor_init (MockNetworkMonitor *self)
{

}
