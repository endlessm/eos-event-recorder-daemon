dbus_src = gnome.gdbus_codegen('emer-event-recorder-server',
    sources: event_recorder_server_xml,
    interface_prefix: 'com.endlessm.Metrics.',
    namespace: 'Emer',
    autocleanup: 'all',
)
daemon_sources = [
    'eins-boottime-source.c',
    'eins-boottime-source.h',
    'emer-aggregate-tally.c',
    'emer-aggregate-tally.h',
    'emer-aggregate-timer-impl.c',
    'emer-aggregate-timer-impl.h',
    'emer-boot-id-provider.c',
    'emer-boot-id-provider.h',
    'emer-cache-size-provider.c',
    'emer-cache-size-provider.h',
    'emer-cache-version-provider.c',
    'emer-cache-version-provider.h',
    'emer-circular-file.c',
    'emer-circular-file.h',
    'emer-daemon.c',
    'emer-daemon.h',
    'emer-gzip.c',
    'emer-gzip.h',
    'emer-image-id-provider.c',
    'emer-image-id-provider.h',
    'emer-machine-id-provider.c',
    'emer-machine-id-provider.h',
    'emer-main.c',
    'emer-network-send-provider.c',
    'emer-network-send-provider.h',
    'emer-permissions-provider.c',
    'emer-permissions-provider.h',
    'emer-persistent-cache.c',
    'emer-persistent-cache.h',
    'emer-site-id-provider.c',
    'emer-site-id-provider.h',
    'emer-types.c',
    'emer-types.h',
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