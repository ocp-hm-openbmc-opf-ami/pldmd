project(
    'pldmd',
    'cpp',
    license: 'Apache-2.0',
    version: '0.1',
    default_options: [
        'warning_level=0',
        'werror=false',
        'cpp_std=c++17'
    ],
)


# need fallback option, as with 'modules' parameter
# dependency resolution doesn't work properly with
# '.wrap' provided dependency
boost = dependency('boost', version : '>=1.71',
                    modules: [ 'coroutine', 'context'],
                    fallback : ['boost', 'boost_dep'])

sdbusplus = dependency('sdbusplus')
phosphorlog_dep = dependency('phosphor-logging')
libpldm_dep = dependency('libpldm', required: false)
if not libpldm_dep.found()
    cmake = import('cmake')
    opt_var = cmake.subproject_options()
    opt_var.add_cmake_defines({'BUILD_STANDALONE': true})
    sub_proj = cmake.subproject('libpldm', options: opt_var)
    # message('CMaket targets:\n - ' + '\n - '.join(sub_proj.target_list()))
    libpldm_dep = sub_proj.dependency('pldm_intel')
    meson.override_dependency('libpldm', libpldm_dep)
endif
mctpwplus_dep = dependency('mctpwplus')
dbus_intf_dep = dependency('phosphor-dbus-interfaces')
if not dbus_intf_dep.found()
    dbus_intf_subprj = subproject('phosphor-dbus-interfaces', default_options: 'data_xyz_openbmc_project=false', required: false)
    dbus_intf_dep = dbus_intf_subprj.find_variable('phosphor_dbus_interfaces_dep')
endif

systemd = dependency('systemd', required: true)
systemd_system_unit_dir = systemd.get_pkgconfig_variable(
    'systemdsystemunitdir',
    define_variable: ['prefix', get_option('prefix')])

threads = dependency('threads')

src_files = ['src/pldmd.cpp' ,'src/platform.cpp' ,'src/platform_terminus.cpp' ,'src/platform_association.cpp' ,'src/pdr_manager.cpp' ,'src/numeric_sensor_handler.cpp' ,'src/numeric_sensor.cpp' ,'src/thresholds.cpp' ,'src/state_sensor_handler.cpp' ,'src/pdr_utils.cpp' ,'src/numeric_effecter.cpp' ,'src/numeric_effecter_handler.cpp' ,'src/state_effecter_handler.cpp' ,'src/sdbus-asio.cpp' ,'src/fwu_inventory.cpp' ,'src/fwu_utils.cpp' ,'src/pldm_fwu_image.cpp' ,'src/firmware_update.cpp' ,'src/fru.cpp' ,'src/base.cpp' ,'src/utils.cpp' ,'src/fru_support.cpp']

no_thread_flags = '-DBOOST_ASIO_DISABLE_THREADS'
include_dirs = ['include']
extra_deps = declare_dependency(compile_args: no_thread_flags, include_directories: include_dirs)

deps = [
    boost,
    systemd,
    sdbusplus,
    phosphorlog_dep,
    libpldm_dep,
    mctpwplus_dep,
    dbus_intf_dep,
    extra_deps
]

pldmd_exe = executable('pldmd', src_files, dependencies: deps)