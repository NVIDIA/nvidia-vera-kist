project(
    'template-application',
    'cpp',
    meson_version: '>=1.3.0',
    default_options: [
        'warning_level=3',
        'werror=true',
        'cpp_std=c++20',
        # note: This is allowed in certain contexts, but provides binary size
        # improvements for applications that don't require rtti
        'cpp_rtti=false',
        'b_lto=true',
    ],
    license: 'Apache-2.0',
    version: '0.1',
)

# Validate the c++ Standard
if get_option('cpp_std') != 'c++20'
    error('This project requires c++20 support')
endif

boost = dependency(
    'boost',
    version: '>=1.87.0',
    required: false,
    include_type: 'system',
)
if not boost.found()
    cmake = import('cmake')
    opt = cmake.subproject_options()
    opt.add_cmake_defines(
        {
            'BOOST_INCLUDE_LIBRARIES': 'asio;callable_traits;container;headers',
            'BUILD_SHARED_LIBS': 'OFF',
        },
    )

    boost = cmake.subproject('boost', required: true, options: opt)
    boost_asio = boost.dependency('boost_asio').as_system()
    boost_callable_traits = boost.dependency('boost_callable_traits').as_system()
    boost_container = boost.dependency('boost_container').as_system()
    boost_headers = boost.dependency('boost_headers').as_system()
    boost = [boost_asio, boost_callable_traits, boost_container, boost_headers]
endif
add_project_arguments(
    '-DBOOST_ASIO_DISABLE_THREADS',
    '-DBOOST_NO_RTTI',
    '-DBOOST_NO_TYPEID',
    language : 'cpp'
)

systemd = dependency('systemd')
sdbusplus = dependency('sdbusplus', required: false, include_type: 'system')
if not sdbusplus.found()
    sdbusplus_proj = subproject('sdbusplus', required: true)
    sdbusplus = sdbusplus_proj.get_variable('sdbusplus_dep')
    sdbusplus = sdbusplus.as_system('system')
endif
add_project_arguments(
    '-DSDBUSPLUS_DISABLE_BOOST_COROUTINES',
    language : 'cpp'
)

cxx = meson.get_compiler('cpp')

systemd_system_unit_dir = systemd.get_variable(
    pkgconfig: 'systemdsystemunitdir'
)

systemd_unit_configuration = configuration_data()
systemd_unit_configuration.set('INSTALL_PREFIX', get_option('prefix'))

configure_file(input: 'xyz.openbmc_project.templateapp.service',
               output: 'xyz.openbmc_project.templateapp.service',
               configuration: systemd_unit_configuration,
               install_dir: systemd_system_unit_dir,
               install: true)

template_app_a = static_library(
    'template_app_a',
    'src/template_app.cpp',
    include_directories: 'include',
)

executable(
    'template-app',
    'src/template_app_main.cpp',
    dependencies: [
      boost,
      sdbusplus,
    ],
    include_directories: 'include',
    install: true,
    link_with: [
        template_app_a,
    ],
)

if get_option('tests').enabled()
    gtest_dep = dependency('gtest', main: true, disabler: true, required: false, include_type: 'system')
    gmock_dep = dependency('gmock', disabler: true, required: false, include_type: 'system')
    if not gtest_dep.found() or not gmock_dep.found()
        gtest_proj = import('cmake').subproject('gtest', required: false)
        if gtest_proj.found()
            gtest_dep = declare_dependency(
                dependencies: [
                    dependency('threads'),
                    gtest_proj.dependency('gtest'),
                    gtest_proj.dependency('gtest_main'),
                ]
            )
            gmock_dep = gtest_proj.dependency('gmock')
        else
            assert(
                not get_option('tests').enabled(),
                'Googletest is required if tests are enabled'
            )
        endif
    endif
    template_app_test = executable(
      'template-app-test',
      'test/template_app_test.cpp',
      dependencies: [
          boost,
          sdbusplus,
          gmock_dep,
          gtest_dep,
      ],
      include_directories: [
          'include',
      ],
      link_with: [
          template_app_a,
      ],
  )

  test('test_template_app', template_app_test)
endif
