libexecdir_config = configuration_data({
    'libexecdir': libexec_dir,
})

# Systemd unit
configure_file(
    input: 'eos-metrics-event-recorder.service.in',
    output: 'eos-metrics-event-recorder.service',
    configuration: libexecdir_config,
    install: true,
    install_dir: systemd_dep.get_pkgconfig_variable('systemdsystemunitdir'),
)

# D-Bus service
configure_file(
    input: 'com.endlessm.Metrics.service.in',
    output: 'com.endlessm.Metrics.service',
    configuration: libexecdir_config,
    install: true,
    install_dir: get_option('datadir') / 'dbus-1' / 'system-services',
)

# D-Bus policy
install_data(
    'com.endlessm.Metrics.conf',
    install_dir: get_option('datadir') / 'dbus-1' / 'system.d',
)

# tmpfiles rules
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

# Daemon configuration
install_data(
    'eos-metrics-permissions.conf',
    install_dir: config_dir,
    install_mode: ['rwxrwsr-x', 'metrics', 'metrics'],
)
install_data(
    'cache-size.conf',
    install_dir: config_dir,
    install_mode: ['rw-r--r--'],
)

# Polkit policy
install_data(
    'com.endlessm.Metrics.policy',
    install_dir: polkit_gobject_dep.get_pkgconfig_variable('policydir'),
)
