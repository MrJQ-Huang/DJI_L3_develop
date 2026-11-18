cmake_minimum_required(VERSION 3.16)
project(l3_metadata_inspect C)

find_package(Threads REQUIRED)
add_library(l3_stream STATIC l3_stream.c)
target_include_directories(l3_stream PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(l3_stream PUBLIC c_std_11)
target_compile_options(l3_stream PRIVATE -Wall -Wextra -Werror)
target_link_libraries(l3_stream PRIVATE Threads::Threads)
add_executable(l3_video_relay l3_video_relay.c)
target_link_libraries(l3_video_relay PRIVATE l3_stream Threads::Threads)
target_compile_options(l3_video_relay PRIVATE -Wall -Wextra -Werror)
include(CTest)
if(BUILD_TESTING)
    add_executable(test_usb_padding tests/test_usb_padding.c l3_usb_padding.c)
    target_include_directories(test_usb_padding PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(test_usb_padding PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME usb_padding COMMAND test_usb_padding)
    add_executable(test_l3_stream tests/test_l3_stream.c)
    target_link_libraries(test_l3_stream PRIVATE l3_stream Threads::Threads)
    target_compile_options(test_l3_stream PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME l3_stream COMMAND test_l3_stream)
    set_tests_properties(l3_stream PROPERTIES TIMEOUT 30)
endif()
option(L3_BUILD_PSDK_APP "Build Manifold PSDK app alongside portable transport" ON)
if(NOT L3_BUILD_PSDK_APP)
    return()
endif()

# Standalone target: NEVER add_subdirectory() the official comprehensive sample.
# Configure/build only; this project has no run/install/DPK or post-build actions.
set(PSDK_ROOT "" CACHE PATH "Official, unmodified DJI Payload-SDK 3.18.0 root")
option(L3_FIX_MOP_USB_PADDING "Fix the pinned ARM64 SDK USB-copy length in a separate build archive" ON)
option(L3_ENABLE_POINT_RECORDING_PROBE
    "Enable point-recording test ONLY after the SDK MOP memory error has a verified fix" OFF)
option(L3_USE_OFFICIAL_SOCKET
    "Use official blocking socket adapter for SDK diagnosis (system writes suppressed)" OFF)
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
# l3_video_control uses the camera transport ABI from this exact SDK archive.
# Validate independently of the optional, unrelated USB padding correction.
file(SHA256 "${PSDK_LIBRARY}" L3_SOURCE_SDK_SHA256)
if(NOT L3_SOURCE_SDK_SHA256 STREQUAL "5725cc72824d915410be59e98cb31b4d269510c4615c1033da8d5fffbffdb5e7")
    message(FATAL_ERROR "L3 source control requires the pinned official PSDK 3.18 ARM64 archive")
endif()
if(L3_FIX_MOP_USB_PADDING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(BUILD_TESTING)
        add_test(NAME sdk_patch_structure COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_psdk_usb_patch.py")
    endif()
    set(PSDK_FIXED_LIBRARY "${CMAKE_CURRENT_BINARY_DIR}/libpayloadsdk_l3_usb_fixed.a")
    add_custom_command(OUTPUT "${PSDK_FIXED_LIBRARY}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_psdk_usb_padding.py"
                --input "${PSDK_LIBRARY}" --output "${PSDK_FIXED_LIBRARY}" --ar "${CMAKE_AR}"
        DEPENDS "${PSDK_LIBRARY}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_psdk_usb_padding.py"
        VERBATIM)
    add_custom_target(l3_fixed_psdk DEPENDS "${PSDK_FIXED_LIBRARY}")
    set(PSDK_LIBRARY "${PSDK_FIXED_LIBRARY}")
endif()
if(BUILD_TESTING)
    add_executable(test_osal tests/test_osal.c l3_osal.c)
    target_include_directories(test_osal PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PSDK_ROOT}/psdk_lib/include" "${PSDK_PLATFORM}/common")
    target_compile_options(test_osal PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(test_osal PRIVATE Threads::Threads)
    add_test(NAME osal_lifecycle COMMAND test_osal)
    set_tests_properties(osal_lifecycle PROPERTIES TIMEOUT 5)
    add_executable(test_pointcloud_reader l3_pointcloud_psdk.c tests/test_pointcloud_reader.c)
    target_include_directories(test_pointcloud_reader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PSDK_ROOT}/psdk_lib/include")
    target_compile_options(test_pointcloud_reader PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME pointcloud_reader COMMAND test_pointcloud_reader)
    add_executable(test_point_probe_controller l3_point_probe_controller.c tests/test_point_probe_controller.c)
    target_include_directories(test_point_probe_controller PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PSDK_ROOT}/psdk_lib/include")
    target_link_libraries(test_point_probe_controller PRIVATE Threads::Threads)
    target_compile_options(test_point_probe_controller PRIVATE -Wall -Wextra -Werror)
    foreach(scenario IN ITEMS normal reader-block late-start start-error stop-failure stop-block wrong-type pre-cancel prepare-cancel sink-failure continuous)
        add_test(NAME controller_${scenario} COMMAND test_point_probe_controller ${scenario})
        set_tests_properties(controller_${scenario} PROPERTIES TIMEOUT 10)
    endforeach()
    add_executable(test_diagnostic_modes tests/test_diagnostic_modes.c)
    target_include_directories(test_diagnostic_modes PRIVATE
        "${PSDK_ROOT}/psdk_lib/include" "${PSDK_PLATFORM}/common" "${PSDK_PLATFORM}/manifold3/hal")
    target_compile_definitions(test_diagnostic_modes PRIVATE _GNU_SOURCE)
    target_compile_options(test_diagnostic_modes PRIVATE
        -Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
    # The included application main is discarded; all exercised SDK calls are
    # explicit stubs in the test. No vendor binary or hardware I/O is linked.
    target_link_options(test_diagnostic_modes PRIVATE -Wl,--gc-sections)
    target_link_libraries(test_diagnostic_modes PRIVATE l3_stream Threads::Threads)
    target_sources(test_diagnostic_modes PRIVATE l3_video_control.c)
    add_test(NAME diagnostic_modes COMMAND test_diagnostic_modes)
    set_tests_properties(diagnostic_modes PROPERTIES TIMEOUT 10)
    add_executable(test_video_control tests/test_video_control.c l3_video_control.c)
    target_include_directories(test_video_control PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PSDK_ROOT}/psdk_lib/include")
    target_compile_options(test_video_control PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME video_control COMMAND test_video_control)
    set_tests_properties(video_control PROPERTIES TIMEOUT 10)
    add_executable(test_rgb_restart tests/test_rgb_restart.c l3_rgb_probe.c)
    target_include_directories(test_rgb_restart PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PSDK_ROOT}/psdk_lib/include")
    target_compile_options(test_rgb_restart PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(test_rgb_restart PRIVATE Threads::Threads)
    add_test(NAME rgb_restart COMMAND test_rgb_restart)
    set_tests_properties(rgb_restart PROPERTIES TIMEOUT 5)
endif()
set(L3_SOCKET_SOURCE metadata_socket.c)
if(L3_USE_OFFICIAL_SOCKET)
    set(L3_SOCKET_SOURCE "${PSDK_PLATFORM}/common/osal/osal_socket.c")
    # The official adapter tries to change global rmem sysctls with system().
    # Retain its socket behavior without executing any shell/system changes.
    set_source_files_properties("${L3_SOCKET_SOURCE}" PROPERTIES
        COMPILE_DEFINITIONS "system=L3Diagnostic_BlockedSystem")
endif()
add_executable(l3_metadata_inspect
    metadata_inspect.c
    ${L3_SOCKET_SOURCE}
    l3_rgb_probe.c
    l3_video_control.c
    l3_usb_padding.c
    l3_osal.c
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
    L3_FIX_MOP_USB_PADDING=$<BOOL:${L3_FIX_MOP_USB_PADDING}>
    L3_ENABLE_POINT_RECORDING_PROBE=$<BOOL:${L3_ENABLE_POINT_RECORDING_PROBE}>
    L3_USE_OFFICIAL_SOCKET=$<BOOL:${L3_USE_OFFICIAL_SOCKET}>)
target_compile_options(l3_metadata_inspect PRIVATE -Wall -Wextra)
target_link_libraries(l3_metadata_inspect PRIVATE l3_stream "${PSDK_LIBRARY}" Threads::Threads m dl)
if(L3_FIX_MOP_USB_PADDING)
    add_dependencies(l3_metadata_inspect l3_fixed_psdk)
endif()
# Do not enable LIBUSB_INSTALLED: this target uses Manifold's existing functionFS
# device endpoints. No USB host claiming, sysctl, networking setup, or chmod.
