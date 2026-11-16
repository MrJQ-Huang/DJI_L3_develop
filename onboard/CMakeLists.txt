cmake_minimum_required(VERSION 3.16)
project(l3_metadata_inspect C)

# Standalone target: NEVER add_subdirectory() the official comprehensive sample.
# Configure/build only; this project has no run/install/DPK or post-build actions.
set(PSDK_ROOT "" CACHE PATH "Official, unmodified DJI Payload-SDK 3.18.0 root")
option(L3_ENABLE_POINT_RECORDING_PROBE
    "Enable point-recording test ONLY after the SDK MOP memory error has a verified fix" OFF)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "Build on aarch64 Linux / Manifold 3 (or explicit Linux aarch64 toolchain).")
endif()
if(NOT EXISTS "${PSDK_ROOT}/psdk_lib/include/dji_core.h")
    message(FATAL_ERROR "Set -DPSDK_ROOT=/path/to/official/Payload-SDK-3.18.0")
endif()
set(PSDK_PLATFORM "${PSDK_ROOT}/samples/sample_c/platform/linux")
set(PSDK_LIBRARY "${PSDK_ROOT}/psdk_lib/lib/aarch64-linux-gnu-gcc/libpayloadsdk.a")
if(NOT EXISTS "${PSDK_LIBRARY}")
    message(FATAL_ERROR "Official aarch64-linux-gnu-gcc/libpayloadsdk.a missing")
endif()
find_package(Threads REQUIRED)
add_executable(l3_metadata_inspect
    metadata_inspect.c
    metadata_socket.c
    l3_rgb_probe.c
    l3_point_probe_controller.c
    l3_pointcloud_psdk.c
    l3p1_tcp_sink.c
    "${PSDK_PLATFORM}/common/osal/osal.c"
    "${PSDK_PLATFORM}/common/osal/osal_fs.c"
    "${PSDK_PLATFORM}/manifold3/hal/hal_usb_bulk.c"
)
target_include_directories(l3_metadata_inspect PRIVATE
    "${PSDK_ROOT}/psdk_lib/include"
    "${PSDK_PLATFORM}/common"
    "${PSDK_PLATFORM}/manifold3/hal"
)
target_compile_features(l3_metadata_inspect PRIVATE c_std_11)
target_compile_definitions(l3_metadata_inspect PRIVATE _GNU_SOURCE SYSTEM_ARCH_LINUX PLATFORM_ARCH_aarch64=1)
target_compile_definitions(l3_metadata_inspect PRIVATE
    L3_ENABLE_POINT_RECORDING_PROBE=$<BOOL:${L3_ENABLE_POINT_RECORDING_PROBE}>)
target_compile_options(l3_metadata_inspect PRIVATE -Wall -Wextra)
target_link_libraries(l3_metadata_inspect PRIVATE "${PSDK_LIBRARY}" Threads::Threads m dl)
# Do not enable LIBUSB_INSTALLED: this target uses Manifold's existing functionFS
# device endpoints. No USB host claiming, sysctl, networking setup, or chmod.
