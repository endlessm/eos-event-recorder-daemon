libexecdir_config = configuration_data({
    'libexecdir': libexec_dir,
})

configure_file(
    input: 'eos-metrics-event-recorder.service.in',
    output: 'eos-metrics-event-recorder.service',
    configuration: libexecdir_config,
    install: true,
    install_dir: systemd_dep.get_pkgconfig_variable('systemdsystemunitdir'),
)

configure_file(
    input: 'com.endlessm.Metrics.service.in',
    output: 'com.endlessm.Metrics.service',
    configuration: libexecdir_config,
    install: true,
    install_dir: dbus_dep.get_pkgconfig_variable('system_bus_services_dir'),
)

configure_file(
    input: 'eos-metrics.conf.in',
    output: 'eos-metrics.conf',
    configuration: configuration_data({
        'configdir': config_dir,
        'persistentcachedir': persistent_cache_dir,
    }),
    install: true,
    install_dir: systemd_dep.get_pkgconfig_variable('tmpfilesdir'),
)

install_data(
    'com.endlessm.Metrics.policy',
    # dbus-1.pc doesn't provide this, grr
    install_dir: dbus_dep.get_pkgconfig_variable('datadir') / 'dbus-1' / 'system.d',
)

install_data(
    'eos-metrics-permissions.conf',
    install_dir: config_dir,
    install_mode: ['rwxrwsr-x', 'metrics', 'metrics'],
)
