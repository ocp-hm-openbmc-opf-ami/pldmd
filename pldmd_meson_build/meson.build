project(
    'pldm',
    'cpp',
    license: 'Apache-2.0',
    version: '0.1',
    default_options: [
        'warning_level=0',
        'werror=false',
        'cpp_std=c++17'
    ],
)

#build_tests = get_option('tests')
yocto_build = get_option('yocto_dep')

dep_required = false

dep_always_required = true

if yocto_build.enabled()
    dep_required = true
endif

cpp = meson.get_compiler('cpp')



boost_args = [
    '-DBOOST_SYSTEM_NO_DEPRECATED',
    '-DBOOST_ERROR_CODE_HEADER_ONLY',
    '-DBOOST_NO_RTTI',
    '-DBOOST_NO_TYPEID',
    '-DBOOST_ALL_NO_LIB',
]

boost = dependency('boost', required: dep_required, version : '>=1.71', modules: [ 'coroutine', 'context'])
if not boost.found()
	 subproject('boost_1_76_0', required: dep_always_required)
     #boost = boost_proj.get_variable('boost_dep')
	 boost = declare_dependency(
	 include_directories: 'subprojects/boost_1_76_0/install/include',
	 link_args:['-Lsubprojects/boost_1_76_0/install/lib', '-lboost_context', '-lboost_coroutine']
    )
endif

sdbusplus = dependency('sdbusplus', required: dep_required)
if not sdbusplus.found()
    sdbusplus_proj = subproject('sdbusplus', required: dep_always_required)
    sdbusplus = sdbusplus_proj.get_variable('sdbusplus_dep')
endif
sdbusplus_dep = sdbusplus.partial_dependency(link_args: true,
     includes : true, links: true)

if cpp.has_header('phosphor-logging/log.hpp')
    phosphorlog_dep = declare_dependency()
else
    subproject('phosphor-logging', required: dep_always_required)
    phosphorlog_dep = declare_dependency(
        include_directories: 'subprojects/phosphor-logging'
    )
endif

systemd = dependency('systemd', required: dep_required)
systemd_system_unit_dir = systemd.get_pkgconfig_variable(
    'systemdsystemunitdir',
    define_variable: ['prefix', get_option('prefix')])

if cpp.has_header('nlohmann/json.hpp')
    nlohmann_json = declare_dependency()
    message('nlohmann json already found')
else
    nlohmann_json = dependency('', required: yocto_build.enabled())
    subproject('nlohmann', required: dep_always_required)
    nlohmann_json = declare_dependency(
      include_directories: [
          'subprojects/nlohmann/single_include',
          'subprojects/nlohmann/single_include/nlohmann'])
endif

configure_file(
  copy: true,
  input: 'service_files/xyz.openbmc_project.pldmd.service',
  install: true,
  install_dir: systemd_system_unit_dir,
  output: 'xyz.openbmc_project.pldmd.service',
)

threads = dependency('threads')

cmake = import('cmake')

mctpwrapper_dep = dependency('mctpwplus', required: dep_required,
    allow_fallback: false)
if not mctpwrapper_dep.found()
     mctpwrapper_proj = subproject('mctpwplus', required: dep_always_required)
     mctpwrapper_dep = mctpwrapper_proj.get_variable('wrapperlib_dep')
endif

deps = [
    boost,
    sdbusplus_dep,
    phosphorlog_dep,
    threads,
    mctpwrapper_dep,
    nlohmann_json
]

src_files = ['src/base.cpp', 'src/firmware_update.cpp', 'src/fru.cpp', 
'src/fwu_inventory.cpp', 'src/fwu_utils.cpp', 'src/numeric_effecter.cpp', 'src/numeric_effecter_handler.cpp',
'src/numeric_sensor.cpp', 'src/numeric_sensor_handler.cpp', 'src/pdr_manager.cpp', 'src/pdr_utils.cpp',
'src/platform.cpp', 'src/platform_terminus.cpp', 'src/pldm_fwu_image.cpp',
'src/pldmd.cpp', 'src/sdbus-asio.cpp', 'src/state_effecter_handler.cpp', 'src/state_sensor_handler.cpp',
'src/thresholds.cpp', 'src/utils.cpp']

exe_options = ['warning_level=3']
if yocto_build.enabled()
    exe_options = ['warning_level=3', 'werror=false']
endif

inc_dir = include_directories('../include/', 'subprojects/mctpwplus/libpldm_intel')

executable(
      'pldm',
      src_files,
      implicit_include_directories: false,
      dependencies: deps,
	  include_directories: inc_dir,
      install: true,
      install_dir: get_option('bindir'),
      override_options : exe_options)
