print_persistent_cache_sources = [
    '../daemon/emer-boot-id-provider.c',
    '../daemon/emer-boot-id-provider.h',
    '../daemon/emer-cache-size-provider.c',
    '../daemon/emer-cache-size-provider.h',
    '../daemon/emer-cache-version-provider.c',
    '../daemon/emer-cache-version-provider.h',
    '../daemon/emer-circular-file.c',
    '../daemon/emer-circular-file.h',
    '../daemon/emer-persistent-cache.c',
    '../daemon/emer-persistent-cache.h',
    'print-persistent-cache.c'
]

executable('print-persistent-cache',
    print_persistent_cache_sources,
    dependencies: [
        emer_required_modules,
        emer_shared_dep,
    ],
    include_directories: include_directories('../daemon'),
    install: false,
)

install_data('eos-metrics-collector.exe',
    install_dir: get_option('libexecdir'),
    install_mode: 'rwxr-xr-x',
)

install_data('eos-upload-metrics',
    install_dir: get_option('bindir'),
    install_mode: 'rwxr-xr-x',
)

foreach p : ['eos-enable-metrics-uploading', 'eos-select-metrics-env']
    configure_file(
        input: p + '.in',
        output: p,
        configuration: configuration_data({
            'permissions_file': permissions_file,
        }),
        install: true,
        install_dir: get_option('bindir'),
        install_mode: 'rwxr-xr-x',
    )
endforeach
