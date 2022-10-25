dbus_src = gnome.gdbus_codegen('emer-event-recorder-server',
    sources: event_recorder_server_xml,
    interface_prefix: 'com.endlessm.Metrics.',
    namespace: 'Emer',
    autocleanup: 'all',
)
daemon_sources = [
    'eins-boottime-source.c',
    'emer-aggregate-tally.c',
    'emer-aggregate-timer-impl.c',
    'emer-boot-id-provider.c',
    'emer-cache-size-provider.c',
    'emer-cache-version-provider.c',
    'emer-circular-file.c',
    'emer-daemon.c',
    'emer-gzip.c',
    'emer-image-id-provider.c',
    'emer-main.c',
    'emer-permissions-provider.c',
    'emer-persistent-cache.c',
    'emer-site-id-provider.c',
    'emer-types.c',
    dbus_src,
]

daemon = executable('eos-metrics-event-recorder',
    daemon_sources,
    dependencies: [
        emer_required_modules,
        emer_shared_dep,
    ],
    include_directories: [
        config_inc,
    ],
    install: true,
    install_dir: get_option('libexecdir'),
)
