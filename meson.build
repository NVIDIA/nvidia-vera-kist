project(
    'nvidia-vera-kist',
    ['cpp', 'c'],
    meson_version: '>=1.3.0',
    default_options: [
        'cpp_std=c++23',
        'c_std=gnu18',
        'werror=true',
        'warning_level=3',
        'buildtype=debugoptimized',
        # note: This is allowed in certain contexts, but provides binary size
        # improvements for applications that don't require rtti
        'cpp_rtti=false',
        'b_lto=true',
    ],
    license: 'Apache-2.0',
    version: '0.1',
)

# Validate the c++ Standard
if get_option('cpp_std') != 'c++23'
    error('This project requires c++23 support')
endif

add_global_arguments(['-Wno-gnu-zero-variadic-macro-arguments'], language: 'c')

# Prefer system Boost (for Yocto/OpenBMC and normal distros)
boost_deps = []

boost_sys = dependency(
    'boost',
    version: '>=1.87.0',
    required: false,
    include_type: 'system',
)
if boost_sys.found()
    boost_deps += boost_sys
else
    # Standalone/non-cross build: fall back to Boost subproject via wrap
    cmake = import('cmake')
    opt = cmake.subproject_options()
    opt.add_cmake_defines(
        {
            'BOOST_INCLUDE_LIBRARIES': 'asio;callable_traits;container;headers',
            'BUILD_SHARED_LIBS': 'OFF',
        },
    )

    boost_sp = cmake.subproject('boost', required: true, options: opt)
    boost_deps += [
        boost_sp.dependency('boost_asio').as_system(),
        boost_sp.dependency('boost_callable_traits').as_system(),
        boost_sp.dependency('boost_container').as_system(),
        boost_sp.dependency('boost_headers').as_system(),
    ]
endif
boost = boost_deps

add_project_arguments(
    '-DBOOST_ASIO_DISABLE_THREADS',
    '-DBOOST_NO_RTTI',
    '-DBOOST_NO_TYPEID',
    language: 'cpp',
)

systemd = dependency('systemd')
sdbusplus = dependency('sdbusplus', required: false, include_type: 'system')
if not sdbusplus.found()
    sdbusplus_proj = subproject('sdbusplus', required: true)
    sdbusplus = sdbusplus_proj.get_variable('sdbusplus_dep')
    sdbusplus = sdbusplus.as_system('system')
endif
add_project_arguments('-DSDBUSPLUS_DISABLE_BOOST_COROUTINES', language: 'cpp')

if get_option('build_itm')
    subdir('itm')
endif

cxx = meson.get_compiler('cpp')

systemd_system_unit_dir = systemd.get_variable(pkgconfig: 'systemdsystemunitdir')

systemd_unit_configuration = configuration_data()
systemd_unit_configuration.set('INSTALL_PREFIX', get_option('prefix'))

configure_file(
    input: 'com.nvidia.vera.ist.service',
    output: 'com.nvidia.vera.ist.service',
    configuration: systemd_unit_configuration,
    install_dir: systemd_system_unit_dir,
    install: true,
)

ist_app_a = static_library(
    'ist_app_a',
    'src/ist_app.cpp',
    include_directories: 'include',
)

executable(
    'ist',
    'src/ist_main.cpp',
    'src/ist_app.cpp',
    dependencies: [boost, sdbusplus],
    include_directories: 'include',
    install: true,
    link_with: [ist_app_a],
)

if get_option('tests').enabled()
    gtest_dep = dependency(
        'gtest',
        main: true,
        disabler: true,
        required: false,
        include_type: 'system',
    )
    gmock_dep = dependency(
        'gmock',
        disabler: true,
        required: false,
        include_type: 'system',
    )
    if not gtest_dep.found() or not gmock_dep.found()
        gtest_proj = import('cmake').subproject('gtest', required: false)
        if gtest_proj.found()
            gtest_dep = declare_dependency(
                dependencies: [
                    dependency('threads'),
                    gtest_proj.dependency('gtest'),
                    gtest_proj.dependency('gtest_main'),
                ],
            )
            gmock_dep = gtest_proj.dependency('gmock')
        else
            assert(
                not get_option('tests').enabled(),
                'Googletest is required if tests are enabled',
            )
        endif
    endif

    ist_app_test = executable(
        'ist-app-test',
        ['test/ist_app_test.cpp', 'src/ist_app.cpp'],
        dependencies: [boost, sdbusplus, gmock_dep, gtest_dep],
        include_directories: ['include'],
        link_with: [ist_app_a],
    )

    test('ist_app_test', ist_app_test)
endif
