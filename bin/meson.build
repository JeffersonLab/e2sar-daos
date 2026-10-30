app_boost_dep = dependency('boost', modules: ['program_options'], required: true)

# The E2SAR RPM ships libe2sar as a static archive, and e2sar.pc lists its Boost libraries
# only under Libs.private. Link them explicitly, as *shared* libraries: libe2sar.a was built
# against shared Boost.Log, whose ABI namespace (v2_mt_posix) differs from the static one.
e2sar_boost_dep = dependency('boost',
    modules: ['log', 'log_setup', 'thread', 'filesystem', 'chrono'], required: true)

# Meson's Boost module table predates Boost.URL, so find it next to the other Boost libraries.
boost_root = run_command('sh', '-c', 'echo "$BOOST_ROOT"', check: false).stdout().strip()
e2sar_sysroot = run_command('sh', '-c', 'echo "$PKG_CONFIG_SYSROOT_DIR"', check: false).stdout().strip()
e2sar_prefix = e2sar_sysroot + e2sar_dep.get_variable(pkgconfig: 'prefix')
boost_url_dirs = [e2sar_prefix / 'lib', e2sar_prefix / 'lib64']
if boost_root != ''
    boost_url_dirs += [boost_root / 'lib', boost_root / 'lib64']
endif
boost_url_dep = cpp_compiler.find_library('boost_url', dirs: boost_url_dirs, required: true)

# libe2sar.a can reference gRPC TLS/insecure helper symbols that are not pulled in by
# the generic `-lgrpc++` / `-lgrpc` pair alone when a staging-root install is used.
# Link the insecure/TLS variants from the same E2SAR prefix before falling back to the
# distro libraries, so Meson does not accidentally pick up a mismatched system gRPC ABI.
grpc_lib_dirs = [e2sar_prefix / 'lib', e2sar_prefix / 'lib64']
if e2sar_sysroot != ''
    grpc_lib_dirs += [e2sar_sysroot / 'usr/local/lib', e2sar_sysroot / 'usr/local/lib64']
endif
grpc_unsecure_dep = cpp_compiler.find_library('grpc++_unsecure', dirs: grpc_lib_dirs + ['/usr/lib64'], required: false)
grpc_core_unsecure_dep = cpp_compiler.find_library('grpc_unsecure', dirs: grpc_lib_dirs + ['/usr/lib64'], required: false)

grpc_link_deps = []
if grpc_unsecure_dep.found()
    grpc_link_deps += [grpc_unsecure_dep]
endif
if grpc_core_unsecure_dep.found()
    grpc_link_deps += [grpc_core_unsecure_dep]
endif

executable('e2sar_daos_recv', 'e2sar_daos_recv.cpp',
    dependencies: [e2sar_daos_dep, e2sar_dep, app_boost_dep, e2sar_boost_dep, boost_url_dep,
                   grpc_link_deps, dependency('threads')],
    install: true
)

# Read-only DAOS run monitor for the live dashboard (viz/daos_monitor/).
executable('e2sar_daos_monitor', 'e2sar_daos_monitor.cpp',
    dependencies: [e2sar_daos_dep, app_boost_dep],
    install: true
)

# Reader/verifier; SHA-256 comes from OpenSSL (openssl-devel).
libcrypto_app_dep = dependency('libcrypto', required: false)
if libcrypto_app_dep.found()
    executable('e2sar_daos_read', 'e2sar_daos_read.cpp',
        dependencies: [e2sar_daos_dep, app_boost_dep, libcrypto_app_dep],
        install: true
    )
    # Synthetic EVIO time-frame sender for end-to-end tests (see docs/howto/e2e-bench.md).
    executable('e2sar_evio_send', 'e2sar_evio_send.cpp',
        dependencies: [e2sar_dep, app_boost_dep, e2sar_boost_dep, boost_url_dep, libcrypto_app_dep,
                       grpc_link_deps, dependency('threads')],
        install: true
    )
else
    message('libcrypto (openssl-devel) not found, skipping e2sar_daos_read and e2sar_evio_send')
endif
