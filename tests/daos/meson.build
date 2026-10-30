test_sources = files('test_daos_version.cpp')

daos_test = executable('daos_test', test_sources,
                            include_directories: project_include_dir,
                            dependencies: [boost_dep, daos_dep])


test('DAOSNativeTests', daos_test, suite: ['daos', 'unit'])

# Event -> KV key/chunk layout used by e2sar_daos_recv / e2sar_daos_read (no DAOS server needed)
event_layout_test = executable('event_layout_test', files('test_event_layout.cpp'),
                                include_directories: project_include_dir,
                                dependencies: [boost_dep])

test('EventLayoutTests', event_layout_test, suite: ['daos', 'unit'])

# Per-run object ids and run catalog records (header-only parts; DAOS headers, no server)
run_catalog_test = executable('run_catalog_test', files('test_run_catalog.cpp'),
                                include_directories: project_include_dir,
                                dependencies: [boost_dep, daos_dep])

test('RunCatalogTests', run_catalog_test, suite: ['daos', 'unit'])

# Object ids, batch tables and catalog records of the run layout (header-only; no server)
layout_test = executable('layout_test', files('test_layout.cpp'),
                                include_directories: project_include_dir,
                                dependencies: [boost_dep, daos_dep])

test('LayoutTests', layout_test, suite: ['daos', 'unit'])

# layout_check POOL CONT [--synthetic]: walk and verify a run written by e2sar_daos_recv
executable('layout_check', files('layout_check.cpp'), dependencies: [e2sar_daos_dep])

latency_histogram_test = executable('latency_histogram_test', files('test_latency_histogram.cpp'),
                                include_directories: project_include_dir,
                                dependencies: [boost_dep])

test('LatencyHistogramTests', latency_histogram_test, suite: ['daos', 'unit'])

# Toy KV round-trip (5 x 0.5 MiB, SHA-256 verified) against a live DAOS system.
# Not part of 'unit': it needs a DAOS server.
# Pool/container come from -Ddaos_pool / -Ddaos_cont, exported as DAOS_POOL / DAOS_CONT.
libcrypto_dep = dependency('libcrypto', required: false)

if libcrypto_dep.found()
    daos_kv_test = executable('daos_kv_test', files('test_daos_kv.cpp'),
                                include_directories: project_include_dir,
                                dependencies: [boost_dep, daos_dep, libcrypto_dep])

    daos_env = environment()
    daos_env.set('DAOS_POOL', get_option('daos_pool'))
    daos_env.set('DAOS_CONT', get_option('daos_cont'))

    test('DAOSKVTest', daos_kv_test, env: daos_env, args: ['--log_level=message'],
        suite: ['daos', 'integration'])
else
    message('libcrypto (openssl-devel) not found, skipping DAOSKVTest')
endif
