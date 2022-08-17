emer_shared_lib = static_library(
    'libeos-metrics-event-recorder-shared',
    ['metrics-util.c', 'metrics-util.h'],
    dependencies: emer_shared_required_modules,
    include_directories: [
        config_inc,
    ],
)
emer_shared_dep = declare_dependency(
    link_with: emer_shared_lib,
    include_directories: include_directories('..'),
)
