simple_tests = {
    'test-aggregate-tally': [
        '../daemon/emer-aggregate-tally.c',
    ],
    'test-boot-id-provider': [
        '../daemon/emer-boot-id-provider.c',
    ],
    'test-cache-size-provider': [
        '../daemon/emer-cache-size-provider.c',
    ],
    'test-cache-version-provider': [
        '../daemon/emer-cache-version-provider.c',
    ],
    'test-circular-file': [
        '../daemon/emer-circular-file.c',
    ],
    'test-gzip': [
        '../daemon/emer-gzip.c',
    ],
    'test-machine-id-provider': [
        '../daemon/emer-machine-id-provider.c',
        '../daemon/emer-types.c',
    ],
    'test-network-send-provider': [
        '../daemon/emer-network-send-provider.c',
    ],
    'test-permissions-provider': [
        '../daemon/emer-permissions-provider.c',
    ],
    'test-persistent-cache': [
        '../daemon/emer-boot-id-provider.c',
        '../daemon/emer-persistent-cache.c',
        'daemon/mock-cache-version-provider.c',
        'daemon/mock-circular-file.c',
    ],
}

foreach name, sources : simple_tests
    test(name,
        executable(name,
            sources + ['daemon/@0@.c'.format(name)],
            dependencies: [
                emer_required_modules,
                emer_shared_dep,
            ],
            include_directories: include_directories('../daemon'),
            install: false,
        ),
        env: {
            'G_DEBUG': 'fatal-warnings',
        },
    )
endforeach

test_daemon = executable('test-daemon',
    [
        dbus_src,
        '../daemon/eins-boottime-source.c',
        '../daemon/emer-aggregate-tally.c',
        '../daemon/emer-aggregate-timer-impl.c',
        '../daemon/emer-boot-id-provider.c',
        '../daemon/emer-daemon.c',
        '../daemon/emer-gzip.c',
        '../daemon/emer-types.c',
        'daemon/mock-cache-size-provider.c',
        'daemon/mock-image-id-provider.c',
        'daemon/mock-machine-id-provider.c',
        'daemon/mock-network-send-provider.c',
        'daemon/mock-permissions-provider.c',
        'daemon/mock-persistent-cache.c',
        'daemon/mock-site-id-provider.c',
        'daemon/test-daemon.c',
    ],
    c_args: [
        '-DTEST_DIR="@0@/"'.format(meson.current_source_dir()),
    ],
    dependencies: [
        emer_required_modules,
        emer_shared_dep,
    ],
    include_directories: include_directories('../daemon'),
    install: false,
)

test('test-daemon',
    find_program('launch-mock-dbus-tests.sh'),
    args: [test_daemon],
    env: {
        'G_DEBUG': 'fatal-warnings',
    },
    # TODO: These tests are slow because they set the daemon's upload interval
    # to 2 seconds and constantly wait for this time to elapse, rather than
    # mocking the clock or telling the daemon to upload. Sure, *some* of them
    # should exercise the daemon's internal timer, but not all.
    timeout: 60,
)

test('test-opt-out-integration',
    find_program('test-opt-out-integration.py'),
    env: {
        'EMER_PATH': daemon.full_path(),
        'G_DEBUG': 'fatal-warnings',
    },
)
