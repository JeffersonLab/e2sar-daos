# Find Boost (required for unit testing)
boost_dep = dependency('boost',
            modules: ['unit_test_framework'], required: true)
message('  - Boost version: ' + boost_dep.version())

subdir('daos')
