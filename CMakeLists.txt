cmake_minimum_required (VERSION 3.5 FATAL_ERROR)

project (pldmd CXX)

option (BUILD_STANDALONE "Use outside of YOCTO depedencies system" OFF)
option (EXPOSE_BASEBOARD_SENSOR "Expose PLDM sensors in baseboard Redfish Chassis interface" OFF)
option (EXPOSE_CHASSIS "Expose PLDM device as a standalone chassis in Redfish Chassis interface" OFF)

set (BUILD_SHARED_LIBRARIES OFF)
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

set (
    CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} \
    -Werror \
    -Wall \
    -Wextra \
    -Wshadow \
    -Wnon-virtual-dtor \
    -Wold-style-cast \
    -Wcast-align \
    -Wunused \
    -Woverloaded-virtual \
    -Wpedantic \
    -Wconversion \
    -Wmisleading-indentation \
    -Wduplicated-cond \
    -Wduplicated-branches \
    -Wlogical-op \
    -Wnull-dereference \
    -Wuseless-cast \
    -Wdouble-promotion \
    -Wno-strict-aliasing \
    -Wno-cast-align \
    -Wformat=2 \
    -Wno-reorder \
"
)

set (CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})

if (EXPOSE_BASEBOARD_SENSOR AND EXPOSE_CHASSIS)
    message (FATAL_ERROR "EXPOSE_BASEBOARD_SENSOR and EXPOSE_CHASSIS cannot be both enabled")
endif ()

if (EXPOSE_BASEBOARD_SENSOR)
    add_definitions (-DEXPOSE_BASEBOARD_SENSOR)
endif ()
if (EXPOSE_CHASSIS)
    add_definitions (-DEXPOSE_CHASSIS)
endif ()

# Add header and sources here
set (SRC_FILES ${PROJECT_SOURCE_DIR}/src/pldmd.cpp
               ${PROJECT_SOURCE_DIR}/src/platform.cpp
               ${PROJECT_SOURCE_DIR}/src/platform_terminus.cpp
               ${PROJECT_SOURCE_DIR}/src/platform_association.cpp
               ${PROJECT_SOURCE_DIR}/src/pdr_manager.cpp
               ${PROJECT_SOURCE_DIR}/src/numeric_sensor_handler.cpp
               ${PROJECT_SOURCE_DIR}/src/numeric_sensor.cpp
               ${PROJECT_SOURCE_DIR}/src/thresholds.cpp
               ${PROJECT_SOURCE_DIR}/src/state_sensor_handler.cpp
               ${PROJECT_SOURCE_DIR}/src/pdr_utils.cpp
               ${PROJECT_SOURCE_DIR}/src/numeric_effecter.cpp
               ${PROJECT_SOURCE_DIR}/src/numeric_effecter_handler.cpp
               ${PROJECT_SOURCE_DIR}/src/state_effecter_handler.cpp
               ${PROJECT_SOURCE_DIR}/src/sdbus-asio.cpp
               ${PROJECT_SOURCE_DIR}/src/fwu_inventory.cpp
               ${PROJECT_SOURCE_DIR}/src/fwu_utils.cpp
               ${PROJECT_SOURCE_DIR}/src/pldm_fwu_image.cpp
               ${PROJECT_SOURCE_DIR}/src/firmware_update.cpp
               ${PROJECT_SOURCE_DIR}/src/fru.cpp
               ${PROJECT_SOURCE_DIR}/src/base.cpp
               ${PROJECT_SOURCE_DIR}/src/utils.cpp
               ${PROJECT_SOURCE_DIR}/src/fru_support.cpp
)

set (HEADER_FILES ${PROJECT_SOURCE_DIR}/include/pldm.hpp
)

include_directories (${PROJECT_SOURCE_DIR}/include)

set (SERVICE_FILES
     ${PROJECT_SOURCE_DIR}/service_files/xyz.openbmc_project.pldmd.service)

add_executable(${PROJECT_NAME} ${SRC_FILES} ${HEADER_FILES})

target_link_libraries (${PROJECT_NAME} mctpwplus sdbusplus -lsystemd -lpldm_intel
                        -lpthread -lstdc++fs -lphosphor_dbus -lboost_coroutine)

find_package (PkgConfig REQUIRED)
pkg_get_variable(SYSTEMD_SYSTEM_UNIT_DIR systemd systemdsystemunitdir)

install (TARGETS ${PROJECT_NAME} DESTINATION bin)
install (FILES ${SERVICE_FILES} DESTINATION "${SYSTEMD_SYSTEM_UNIT_DIR}")
