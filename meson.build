# -*- mode: meson -*-

# Style objective: be consistent with what mesonbuild.com documents/uses, and/or
# the meson book: https://meson-manual.com/

project(
  'i3',
  'c',
  version: '4.19.2',
  default_options: [
    'c_std=c11',
    'warning_level=1',  # enable all warnings (-Wall)
    # TODO(https://github.com/i3/i3/issues/4087): switch to
    # 'buildtype=debugoptimized',
  ],
  # Ubuntu 18.04 (supported until 2023) has meson 0.45.
  # We can revisit our minimum supported meson version
  # if it turns out to be too hard to maintain.
  meson_version: '>=0.45.0',
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments(['-Wunused-value']), language: 'c')

if meson.version().version_compare('>=0.48.0')
  # https://github.com/mesonbuild/meson/issues/2166#issuecomment-629696911
  meson.add_dist_script('meson/meson-dist-script')
else
  message('meson <0.48.0 detected, dist tarballs will not be filtered')
endif

################################################################################
# Version handling
################################################################################

cdata = configuration_data()

version_array = meson.project_version().split('.')
cdata.set('MAJOR_VERSION', version_array[0].to_int())
cdata.set('MINOR_VERSION', version_array[1].to_int())
if version_array.length() > 2
  cdata.set('PATCH_VERSION', version_array[2].to_int())
else
  cdata.set('PATCH_VERSION', 0)
endif
cdata.set_quoted('I3_VERSION', '@VCS_TAG@')
cdata.set_quoted('SYSCONFDIR', join_paths(get_option('prefix'), get_option('sysconfdir')))

if get_option('b_sanitize').split(',').contains('address')
  cdata.set('I3_ASAN_ENABLED', 1)
endif

cdata.set('HAVE_STRNDUP', cc.has_function('strndup'))
cdata.set('HAVE_MKDIRP', cc.has_function('mkdirp'))

# Instead of generating config.h directly, make vcs_tag generate it so that
# @VCS_TAG@ is replaced.
config_h_in = configure_file(
  output: 'config.h.in',
  configuration: cdata,
)
config_h = declare_dependency(
  sources: vcs_tag(
    input: config_h_in,
    output: 'config.h',
    fallback: meson.project_version() + '-non-git',
  )
)

################################################################################
# docs generation
################################################################################

docdir = get_option('docdir')
if docdir == ''
  docdir = join_paths(get_option('datadir'), 'doc', 'i3')
endif

if get_option('docs')
  asciidoc = find_program('asciidoc')
  doc_toc_inputs = [
    'docs/hacking-howto',
    'docs/userguide',
    'docs/ipc',
    'docs/multi-monitor',
    'docs/wsbar',
    'docs/testsuite',
    'docs/i3bar-protocol',
    'docs/layout-saving',
  ]
  foreach m : doc_toc_inputs
    custom_target(
      m.underscorify()+'_asciidoc',
      input: m,
      output: '@BASENAME@.html',
      command: [
        asciidoc,
        '-a', 'toc',
        '-n',
        '-o', '@OUTPUT@',
        '@INPUT@',
      ],
      install: true,
      install_dir: docdir,
    )
  endforeach

  doc_notoc_inputs = [
    'docs/debugging',
  ]
  foreach m : doc_notoc_inputs
    custom_target(
      m.underscorify()+'_asciidoc',
      input: m,
      output: '@BASENAME@.html',
      command: [
        asciidoc,
        '-n',
        '-o', '@OUTPUT@',
        '@INPUT@',
      ],
      install: true,
      install_dir: docdir,
    )
  endforeach

else
  if run_command('[', '-f', 'docs/hacking-howto.html', ']').returncode() == 0
    install_data(
      [
	'docs/hacking-howto.html',
	'docs/userguide.html',
	'docs/ipc.html',
	'docs/multi-monitor.html',
	'docs/wsbar.html',
	'docs/testsuite.html',
	'docs/i3bar-protocol.html',
	'docs/layout-saving.html',
	'docs/debugging.html',
      ],
      install_dir: docdir,
    )
  endif
endif

install_data(
  [
    'docs/bigpicture.png',
    'docs/single_terminal.png',
    'docs/snapping.png',
    'docs/two_columns.png',
    'docs/two_terminals.png',
    'docs/modes.png',
    'docs/wsbar.png',
    'docs/keyboard-layer1.png',
    'docs/keyboard-layer2.png',
    'docs/i3-sync-working.png',
    'docs/i3-sync.png',
    'docs/tree-layout1.png',
    'docs/tree-layout2.png',
    'docs/tree-shot1.png',
    'docs/tree-shot2.png',
    'docs/tree-shot3.png',
    'docs/tree-shot4.png',
    'docs/refcard.html',
    'docs/refcard_style.css',
    'docs/logo-30.png',
    'docs/layout-saving-1.png',
  ],
  install_dir: docdir,
)

if meson.version().version_compare('>=0.53')
  summary('build docs (-Ddocs)', get_option('docs'))
endif

################################################################################
# manpages
################################################################################

man1 = join_paths(get_option('mandir'), 'man1')

if get_option('mans')
  asciidoc = find_program('asciidoc')
  asciidoc_cdata = configuration_data()
  asciidoc_cdata.set('PACKAGE_VERSION', meson.project_version())
  asciidoc_conf = configure_file(
    input: 'man/asciidoc.conf.in',
    output: 'asciidoc.conf',
    configuration: asciidoc_cdata,
  )

  xmlto = find_program('xmlto')

  pod2man = find_program('pod2man')

  man_inputs = [
    'man/i3.man',
    'man/i3bar.man',
    'man/i3-msg.man',
    'man/i3-input.man',
    'man/i3-nagbar.man',
    'man/i3-config-wizard.man',
    'man/i3-migrate-config-to-v4.man',
    'man/i3-sensible-editor.man',
    'man/i3-sensible-pager.man',
    'man/i3-sensible-terminal.man',
    'man/i3-dump-log.man',
  ]

  foreach m : man_inputs
    xml = custom_target(
      m.underscorify()+'_asciidoc',
      input: m,
      output: '@BASENAME@.xml',
      command: [
        asciidoc,
        '-d', 'manpage',
        '-b', 'docbook',
        '-f', asciidoc_conf,
        '-o', '@OUTPUT@',
        '@INPUT@',
      ],
    )

    custom_target(
      m.underscorify()+'_xmlto',
      input: xml,
      output: '@BASENAME@.1',
      command: [
        xmlto,
        'man',
        '-o',
        '@OUTDIR@',
        '@INPUT@',
      ],
      # We should use install and install_dir instead of install_man as per:
      # https://github.com/mesonbuild/meson/issues/4981#issuecomment-467084867
      # https://github.com/mesonbuild/meson/issues/1550#issuecomment-370164307
      install: true,
      install_dir: man1,
    )
  endforeach

  pod2man_inputs = [
    'i3-dmenu-desktop',
    'i3-save-tree',
  ]
  foreach m : pod2man_inputs
    custom_target(
      m.underscorify()+'_pod2man',
      input: m,
      output: '@BASENAME@.1',
      command: [
        pod2man,
        '--utf8',
        '@INPUT@',
        '@OUTPUT@',
      ],
      # We should use install and install_dir instead of install_man as per:
      # https://github.com/mesonbuild/meson/issues/4981#issuecomment-467084867
      # https://github.com/mesonbuild/meson/issues/1550#issuecomment-370164307
      install: true,
      install_dir: man1,
    )
  endforeach

else
  if run_command('[', '-f', 'man/i3.1', ']').returncode() == 0
    install_data(
      [
	'man/i3.1',
	'man/i3bar.1',
	'man/i3-msg.1',
	'man/i3-input.1',
	'man/i3-nagbar.1',
	'man/i3-config-wizard.1',
	'man/i3-migrate-config-to-v4.1',
	'man/i3-sensible-editor.1',
	'man/i3-sensible-pager.1',
	'man/i3-sensible-terminal.1',
	'man/i3-dump-log.1',
	'man/i3-dmenu-desktop.1',
	'man/i3-save-tree.1',
      ],
      install_dir: man1,
    )
  endif
endif

if meson.version().version_compare('>=0.53')
  summary('build manpages (-Dmans)', get_option('mans'))
endif

# Required for e.g. struct ucred to be defined as per unix(7).
add_project_arguments('-D_GNU_SOURCE', language: 'c')

# https://mesonbuild.com/howtox.html#add-math-library-lm-portably
m_dep = cc.find_library('m', required: false)
rt_dep = cc.find_library('rt', required: false)
iconv_dep = cc.find_library('iconv', required: false)

libsn_dep = dependency('libstartup-notification-1.0', method: 'pkg-config')
xcb_dep = dependency('xcb', method: 'pkg-config')
xcb_xkb_dep = dependency('xcb-xkb', method: 'pkg-config')
xcb_xinerama_dep = dependency('xcb-xinerama', method: 'pkg-config')
xcb_randr_dep = dependency('xcb-randr', method: 'pkg-config')
xcb_shape_dep = dependency('xcb-shape', method: 'pkg-config')
xcb_util_dep = dependency('xcb-util', method: 'pkg-config')
xcb_util_cursor_dep = dependency('xcb-cursor', method: 'pkg-config')
xcb_util_keysyms_dep = dependency('xcb-keysyms', method: 'pkg-config')
xcb_util_wm_dep = dependency('xcb-icccm', method: 'pkg-config')
xcb_util_xrm_dep = dependency('xcb-xrm', method: 'pkg-config')
xkbcommon_dep = dependency('xkbcommon', method: 'pkg-config')
xkbcommon_x11_dep = dependency('xkbcommon-x11', method: 'pkg-config')
yajl_dep = dependency('yajl', method: 'pkg-config')
libpcre_dep = dependency('libpcre', version: '>=8.10', method: 'pkg-config')
cairo_dep = dependency('cairo', version: '>=1.14.4', method: 'pkg-config')
pangocairo_dep = dependency('pangocairo', method: 'pkg-config')
glib_dep = dependency('glib-2.0', method: 'pkg-config')
gobject_dep = dependency('gobject-2.0', method: 'pkg-config')

ev_dep = cc.find_library('ev')

inc = include_directories('include')

libi3srcs = [
  'libi3/boolstr.c',
  'libi3/create_socket.c',
  'libi3/dpi.c',
  'libi3/draw_util.c',
  'libi3/fake_configure_notify.c',
  'libi3/font.c',
  'libi3/format_placeholders.c',
  'libi3/get_colorpixel.c',
  'libi3/get_config_path.c',
  'libi3/get_exe_path.c',
  'libi3/get_mod_mask.c',
  'libi3/get_process_filename.c',
  'libi3/get_visualtype.c',
  'libi3/g_utf8_make_valid.c',
  'libi3/ipc_connect.c',
  'libi3/ipc_recv_message.c',
  'libi3/ipc_send_message.c',
  'libi3/is_debug_build.c',
  'libi3/path_exists.c',
  'libi3/resolve_tilde.c',
  'libi3/root_atom_contents.c',
  'libi3/safewrappers.c',
  'libi3/string.c',
  'libi3/ucs2_conversion.c',
  'libi3/nonblock.c',
  'libi3/screenshot_wallpaper.c',
  'libi3/is_background_set.c',
]

if not cdata.get('HAVE_STRNDUP')
  libi3srcs += 'libi3/strndup.c'
endif

if not cdata.get('HAVE_MKDIRP')
  libi3srcs += 'libi3/mkdirp.c'
endif

libi3 = static_library(
  'i3',
  libi3srcs,
  include_directories: inc,
  dependencies: [
    pangocairo_dep,
    config_h,
  ],
)

i3srcs = [
  'src/assignments.c',
  'src/bindings.c',
  'src/click.c',
  'src/commands.c',
  'src/commands_parser.c',
  'src/con.c',
  'src/config.c',
  'src/config_directives.c',
  'src/config_parser.c',
  'src/display_version.c',
  'src/drag.c',
  'src/ewmh.c',
  'src/fake_outputs.c',
  'src/floating.c',
  'src/handlers.c',
  'src/ipc.c',
  'src/key_press.c',
  'src/load_layout.c',
  'src/log.c',
  'src/main.c',
  'src/manage.c',
  'src/match.c',
  'src/move.c',
  'src/output.c',
  'src/randr.c',
  'src/regex.c',
  'src/render.c',
  'src/resize.c',
  'src/restore_layout.c',
  'src/scratchpad.c',
  'src/sd-daemon.c',
  'src/sighandler.c',
  'src/startup.c',
  'src/sync.c',
  'src/tiling_drag.c',
  'src/tree.c',
  'src/util.c',
  'src/version.c',
  'src/window.c',
  'src/workspace.c',
  'src/x.c',
  'src/xcb.c',
  'src/xcursor.c',
  'src/xinerama.c',
]

# Verify the perl interpreter is present for running parser_gen,
# ensuring a good error message when it isn’t:
perl = find_program('perl')
parser_gen = find_program('generate-command-parser.pl')

command_parser = custom_target(
  'command_parser',
  input: 'parser-specs/commands.spec',
  output: [
    'GENERATED_command_enums.h',
    'GENERATED_command_tokens.h',
    'GENERATED_command_call.h',
  ],
  command: [perl, parser_gen, '--input=@INPUT@', '--prefix=command'],
)

i3srcs += command_parser

config_parser = custom_target(
  'config_parser',
  input: 'parser-specs/config.spec',
  output: [
    'GENERATED_config_enums.h',
    'GENERATED_config_tokens.h',
    'GENERATED_config_call.h',
  ],
  command: [parser_gen, '--input=@INPUT@', '--prefix=config'],
)

i3srcs += config_parser

# src/log.c uses threading primitives for synchronization
thread_dep = dependency('threads')

common_deps = [
  thread_dep,
  m_dep,
  iconv_dep,
  rt_dep,
  libsn_dep,
  xcb_dep,
  xcb_xkb_dep,
  xcb_xinerama_dep,
  xcb_randr_dep,
  xcb_shape_dep,
  xcb_util_dep,
  xcb_util_cursor_dep,
  xcb_util_keysyms_dep,
  xcb_util_wm_dep,
  xcb_util_xrm_dep,
  xkbcommon_dep,
  xkbcommon_x11_dep,
  yajl_dep,
  libpcre_dep,
  cairo_dep,
  pangocairo_dep,
  glib_dep,
  gobject_dep,
  ev_dep,
  config_h,
]

executable(
  'i3',
  i3srcs,
  install: true,
  include_directories: inc,
  dependencies: common_deps,
  link_with: libi3,
)

# This is the only currently working way of installing a symbolic link:
meson.add_install_script(
  'meson/meson-install-i3-with-shmlog',
  get_option('bindir'),
)

executable(
  'i3bar',
  [
    'i3bar/src/child.c',
    'i3bar/src/config.c',
    'i3bar/src/ipc.c',
    'i3bar/src/main.c',
    'i3bar/src/mode.c',
    'i3bar/src/outputs.c',
    'i3bar/src/parse_json_header.c',
    'i3bar/src/workspaces.c',
    'i3bar/src/xcb.c',
  ],
  install: true,
  include_directories: include_directories('include', 'i3bar/include'),
  dependencies: common_deps,
  link_with: libi3,
)

executable(
  'i3-config-wizard',
  [
    'i3-config-wizard/i3-config-wizard-atoms.xmacro.h',
    'i3-config-wizard/main.c',
    'i3-config-wizard/xcb.h',
    config_parser,
  ],
  install: true,
  include_directories: include_directories('include', 'i3-config-wizard'),
  dependencies: common_deps,
  link_with: libi3,
)

executable(
  'i3-dump-log',
  'i3-dump-log/main.c',
  install: true,
  include_directories: inc,
  dependencies: common_deps,
  link_with: libi3,
)

executable(
  'i3-input',
  [
    'i3-input/i3-input.h',
    'i3-input/keysym2ucs.h',
    'i3-input/keysym2ucs.c',
    'i3-input/main.c',
  ],
  install: true,
  include_directories: inc,
  dependencies: common_deps,
  link_with: libi3,
)

executable(
  'i3-msg',
  'i3-msg/main.c',
  install: true,
  include_directories: inc,
  dependencies: common_deps,
  link_with: libi3,
)

executable(
  'i3-nagbar',
  [
    'i3-nagbar/i3-nagbar-atoms.xmacro.h',
    'i3-nagbar/main.c',
  ],
  install: true,
  include_directories: include_directories('include', 'i3-nagbar'),
  dependencies: common_deps,
  link_with: libi3,
)

install_data(
  [
    'i3-dmenu-desktop',
    'i3-migrate-config-to-v4',
    'i3-save-tree',
    'i3-sensible-editor',
    'i3-sensible-pager',
    'i3-sensible-terminal',
  ],
  install_dir: 'bin',
)

install_subdir(
  'etc',
  strip_directory: true,
  install_dir: join_paths(get_option('sysconfdir'), 'i3'),
)

install_subdir(
  'share/',
  strip_directory: true,
  install_dir: get_option('datadir'),
)

install_headers(
  'include/i3/ipc.h',
  subdir: 'i3',
)

# We cannot use configure_file for complete-run.pl.in and i3test.pm.in
# because configure_file strips the backslash in e.g. \@display,
# resulting in @display, breaking our Perl code:
# https://github.com/mesonbuild/meson/issues/7165
bash = find_program('bash')
replace_dirs = [
  bash, '-c',  # Use bash to capture output and mark as executable
  'sed -e \'s,@abs_top_builddir@,'
  + meson.current_build_dir()
  + ',g;s,@abs_top_srcdir@,'
  + meson.current_source_dir()+',g\''
  # Only mark files ending in .pl as executables
  + ' "$0" > "$1" && { [[ "${1##*.}" == pl ]] && chmod +x "$1" || true; }',
  '@INPUT0@',   # $0
  '@OUTPUT0@',  # $1
]
complete_run = custom_target(
  'complete-run',
  input: ['testcases/complete-run.pl.in'],
  output: ['complete-run.pl'],
  command: replace_dirs,
  # build this target when running e.g. ninja or ninja test.
  # This is required for older meson versions (< 0.46.0).
  build_by_default: true,
)
i3test_pm = custom_target(
  'i3test-pm',
  input: ['testcases/lib/i3test.pm.in'],
  output: ['i3test.pm'],
  command: replace_dirs,
  # build this target when running e.g. ninja or ninja test.
  # This is required for older meson versions (< 0.46.0).
  build_by_default: true,
)

if get_option('docs')
  i3_pod2html = find_program('docs/i3-pod2html')

  custom_target(
    'lib-i3test.html',
    input: i3test_pm,
    output: 'lib-i3test.html',
    command: [
      i3_pod2html,
      '@INPUT@',
      '@OUTPUT@',
    ],
    install: true,
    install_dir: docdir,
  )

  custom_target(
    'lib-i3test-test.html',
    input: 'testcases/lib/i3test/Test.pm',
    output: 'lib-i3test-test.html',
    command: [
      i3_pod2html,
      '@INPUT@',
      '@OUTPUT@',
    ],
    install: true,
    install_dir: docdir,
  )
endif

executable(
  'test.inject_randr15',
  'testcases/inject_randr1.5.c',
  include_directories: inc,
  dependencies: common_deps,
  link_with: libi3,
)

executable(
  'test.commands_parser',
  [
    'src/commands_parser.c',
    command_parser,
  ],
  include_directories: inc,
  c_args: '-DTEST_PARSER',
  dependencies: common_deps,
  link_with: libi3,
)

executable(
  'test.config_parser',
  [
    'src/config_parser.c',
    config_parser,
  ],
  include_directories: inc,
  c_args: '-DTEST_PARSER',
  dependencies: common_deps,
  link_with: libi3,
)

anyevent_i3 = custom_target(
  'anyevent-i3',
  # Should be AnyEvent-I3/blib/lib/AnyEvent/I3.pm,
  # but see https://github.com/mesonbuild/meson/issues/2320
  output: 'AnyEvent-I3.stamp',
  command: [
    'sh',
    '-c',
    'cp -r @0@/AnyEvent-I3 . && cd AnyEvent-I3 && perl Makefile.PL && make && touch ../AnyEvent-I3.stamp'.format(meson.current_source_dir()),
  ],
)

if meson.version().version_compare('>=0.46.0')
  test(
    'complete-run',
    perl,
    args: [complete_run],
    depends: [
      anyevent_i3,
      i3test_pm,
    ],
    timeout: 120,  # Default of 30 seconds can cause timeouts on slower machines
  )
else
  # meson < 0.46.0 does not support the depends arg in test targets.
  # Just hope for the best.
  test(
    'complete-run',
    perl,
    args: [complete_run],
  )
  message('meson < 0.46 detected, you might need to run ninja test twice')
endif
