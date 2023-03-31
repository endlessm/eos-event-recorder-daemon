project('eos-event-recorder-daemon',
        'c',
        license: 'GPL-2.0-or-later',
        version: '0.0.0',
        default_options: ['c_std=c11', 'warning_level=2'],
        meson_version: '>= 0.52.0',
)

cc = meson.get_compiler('c')

glib_dep = dependency('glib-2.0', version: '>= 2.68')
eosmetrics_dep = dependency('eosmetrics-0', version: '>= 0.2')
polkit_gobject_dep = dependency('polkit-gobject-1')

emer_shared_required_modules = [
    glib_dep,
    dependency('gio-unix-2.0', version: '>= 2.46'),
    dependency('gobject-2.0', version: '>= 2.58'),
    dependency('sqlite3'),
    dependency('uuid'),
]
emer_required_modules = emer_shared_required_modules + [
    eosmetrics_dep,
    cc.find_library('m', required: false),
    dependency('ostree-1', version: '>= 2013.7'),
    polkit_gobject_dep,
    dependency('libsoup-2.4', version: '>= 2.42'),
]

gnome = import('gnome')
py = import('python').find_installation('python3',
    modules: [
        # Meson can't express dbusmock >= 0.10 here
        'dbusmock',
        'dbus',
    ],
)

systemd_dep = dependency('systemd')
event_recorder_server_xml = eosmetrics_dep.get_variable(pkgconfig: 'datadir') / 'eos-metrics' / 'com.endlessm.Metrics.xml'

default_metrics_server_url = get_option('default_metrics_server_url')

prefix = get_option('prefix')
libexec_dir = join_paths(prefix, get_option('libexecdir'))
config_dir = get_option('sysconfdir') / 'metrics'
permissions_file = config_dir / 'eos-metrics-permissions.conf'
persistent_cache_dir = get_option('localstatedir') / 'cache' / 'metrics' / 'v3'

add_project_arguments(
  [
    '-DCONFIG_DIR="@0@"'.format(config_dir),
    '-DPERMISSIONS_FILE="@0@"'.format(permissions_file),
    '-DPERSISTENT_CACHE_DIR="@0@"'.format(persistent_cache_dir),
    '-DSYSCONFDIR="@0@"'.format(get_option('sysconfdir')),
    '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_68',
    '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_68',
    '-Wno-unused-parameter',
  ],
  language: 'c',
)
conf_data = configuration_data()
conf_data.set_quoted('DEFAULT_METRICS_SERVER_URL', default_metrics_server_url)
configure_file(
    output: 'config.h',
    configuration: conf_data,
)
config_inc = include_directories('.')

# Allow the tests to be easily run under valgrind using --setup=valgrind
valgrind = find_program('valgrind', required: false)
if valgrind.found()
  glib_suppression_file = glib_dep.get_variable(pkgconfig: 'prefix') / 'share/glib-2.0/valgrind/glib.supp'

  valgrind_arguments = [
    '--tool=memcheck',
    '--error-exitcode=1',
    '--track-origins=yes',
    '--leak-check=full',
    '--leak-resolution=high',
    '--num-callers=50',
    '--show-leak-kinds=definite,possible',
    '--show-error-list=yes',
    '--suppressions=@0@'.format(glib_suppression_file),
  ]
  add_test_setup('valgrind',
    exe_wrapper: [valgrind, valgrind_arguments],
    # For a test which runs the entire daemon as a subprocess. We don't use
    # --trace-children=yes because that test also spawns other things such as
    # a dbus-daemon and we don't want to valgrind those.
    env: {
      'EXE_WRAPPER': ' '.join(
        # TODO: use .full_path() once we require meson ≥ 0.55
        [valgrind.path()] + valgrind_arguments
      ),
    },
    timeout_multiplier: 10,
  )
endif

subdir('data')
subdir('shared')
subdir('daemon')
subdir('tools')
subdir('tests')
