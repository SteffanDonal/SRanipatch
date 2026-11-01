cmake_minimum_required(VERSION 3.25)
project(sranipatch VERSION 0.1.0 LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
    configure_file(cmake/icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/icon.rc" @ONLY)
endif()

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Supported platforms are Windows and Linux")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A 64-bit target is required")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

option(SRANIPATCH_BUILD_SERVER "Build camera / MNN / OSC server" ON)
option(SRANIPATCH_BUILD_CLIENT "Build Dear ImGui client" ON)
option(
    SRANIPATCH_BUILD_DRIVER
    "Build SteamVR lifecycle driver"
    ${SRANIPATCH_BUILD_SERVER}
)
option(
    SRANIPATCH_ENABLE_IPO
    "Enable release link-time optimization for application code"
    ON
)
option(
    SRANIPATCH_SANITIZE
    "Enable address and undefined behavior sanitizers"
    OFF
)
include(CTest)
include(cmake/Dependencies.cmake)
find_package(Threads REQUIRED)

add_library(sranipatch_options INTERFACE)
target_compile_features(sranipatch_options INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(
        sranipatch_options
        INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus
    )
    target_compile_definitions(
        sranipatch_options
        INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN
    )
else()
    target_compile_options(
        sranipatch_options
        INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_link_options(sranipatch_options INTERFACE -Wl,-z,noexecstack)
    if(SRANIPATCH_SANITIZE)
        target_compile_options(
            sranipatch_options
            INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(
            sranipatch_options
            INTERFACE -fsanitize=address,undefined
        )
    endif()
endif()

if(SRANIPATCH_ENABLE_IPO AND NOT SRANIPATCH_SANITIZE)
    include(CheckIPOSupported)
    check_ipo_supported(
        RESULT sranipatch_ipo_supported
        OUTPUT sranipatch_ipo_error
    )
endif()
function(sranipatch_target target)
    target_link_libraries(${target} PRIVATE sranipatch_options)
    get_target_property(target_type ${target} TYPE)
    if(WIN32 AND target_type STREQUAL "EXECUTABLE")
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/icon.rc")
    endif()
    if(sranipatch_ipo_supported)
        set_property(
            TARGET ${target}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        )
        set_property(
            TARGET ${target}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
        )
    endif()
endfunction()

add_library(sranipatch_runtime STATIC src/process.cpp src/runtime_control.cpp)
target_include_directories(sranipatch_runtime PUBLIC include)
target_link_libraries(sranipatch_runtime PUBLIC Threads::Threads)
set_target_properties(
    sranipatch_runtime
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)
sranipatch_target(sranipatch_runtime)

add_library(
    sranipatch_core
    STATIC
    src/ipc.cpp
    src/osc.cpp
    src/frame.cpp
    src/config.cpp
    src/live_osc_config.cpp
    src/capture/vive_tracker.cpp
)
target_include_directories(sranipatch_core PUBLIC include)
target_link_libraries(sranipatch_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(sranipatch_core PRIVATE ws2_32)
else()
    target_sources(sranipatch_core PRIVATE src/steam_link.cpp)
endif()
sranipatch_target(sranipatch_core)

if(SRANIPATCH_BUILD_SERVER)
    sranipatch_add_mnn()
    sranipatch_add_opencv()
    if(WIN32)
        set(capture_source src/capture/media_foundation.cpp)
    else()
        set(capture_source src/capture/v4l2.cpp)
    endif()
    add_library(sranipatch_capture STATIC ${capture_source})
    target_link_libraries(sranipatch_capture PUBLIC sranipatch_core)
    sranipatch_target(sranipatch_capture)
    add_library(
        sranipatch_inference
        STATIC
        src/inference/mnn.cpp
        src/inference/lip.cpp
        src/inference/image.cpp
    )
    target_include_directories(
        sranipatch_inference
        SYSTEM
        PRIVATE "${mnn_SOURCE_DIR}/include"
    )
    target_include_directories(
        sranipatch_inference
        SYSTEM
        PRIVATE ${sranipatch_opencv_includes}
    )
    target_link_libraries(
        sranipatch_inference
        PUBLIC sranipatch_core
        PRIVATE MNN opencv_core
    )
    sranipatch_target(sranipatch_inference)
    add_executable(
        sranipatch-server
        src/server/main.cpp
        src/server/tracking.cpp
        src/server/runtime.cpp
    )
    target_link_libraries(
        sranipatch-server
        PRIVATE sranipatch_capture sranipatch_inference MNN sranipatch_runtime
    )
    if(WIN32)
        target_link_libraries(
            sranipatch_capture
            PRIVATE mfplat mfreadwrite mfuuid mf ole32 strmiids ksuser cfgmgr32
        )
    endif()
    sranipatch_target(sranipatch-server)
    add_custom_target(
        sranipatch-model
        COMMAND
            ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:sranipatch-server>/models"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../assets/models/lip_model.mnn"
            "$<TARGET_FILE_DIR:sranipatch-server>/models/lip_model.mnn"
        VERBATIM
    )
    add_dependencies(sranipatch-server sranipatch-model)
    install(TARGETS sranipatch-server RUNTIME DESTINATION bin COMPONENT Runtime)
    install(FILES ../assets/models/lip_model.mnn DESTINATION bin/models COMPONENT Runtime)
    install(
        FILES third_party/vivefacialtracker-LICENSE.txt
        DESTINATION share/licenses
        COMPONENT Runtime
    )
endif()

if(SRANIPATCH_BUILD_CLIENT OR SRANIPATCH_BUILD_DRIVER)
    sranipatch_add_openvr()
endif()

if(SRANIPATCH_BUILD_CLIENT)
    file(
        READ "${CMAKE_CURRENT_SOURCE_DIR}/assets/head.glb"
        head_header
        LIMIT 43
    )
    if(head_header MATCHES "^version https://git-lfs.github.com/spec/v1")
        message(
            FATAL_ERROR
            "assets/head.glb is a Git LFS pointer. Install Git LFS and run 'git lfs pull', then configure again."
        )
    endif()
    sranipatch_add_gui()
    add_library(sranipatch_head_model STATIC src/client/head_model.cpp)
    target_include_directories(sranipatch_head_model PUBLIC src/client)
    target_link_libraries(
        sranipatch_head_model
        PUBLIC sranipatch_core
        PRIVATE sranipatch_cgltf
    )
    sranipatch_target(sranipatch_head_model)
    add_library(sranipatch_observation STATIC src/client/observation.cpp)
    target_include_directories(sranipatch_observation PUBLIC src/client)
    target_link_libraries(sranipatch_observation PUBLIC sranipatch_core)
    sranipatch_target(sranipatch_observation)
    add_executable(
        sranipatch-client
        src/client/main.cpp
        src/client/client_ui.cpp
        src/client/overlay.cpp
        src/client/head_preview.cpp
        src/client/style.cpp
        src/client/parameter_grid.cpp
        src/client/performance_plot.cpp
    )
    target_link_libraries(
        sranipatch-client
        PRIVATE
            sranipatch_core
            sranipatch_implot
            sranipatch_head_model
            sranipatch_glad
            sranipatch_observation
            sranipatch_openvr
            sranipatch_runtime
    )
    sranipatch_target(sranipatch-client)
    if(WIN32)
        set_target_properties(
            sranipatch-client
            PROPERTIES WIN32_EXECUTABLE TRUE
        )
        target_sources(
            sranipatch-client
            PRIVATE src/client/windows_chrome.cpp src/client/windows_surface.cpp
        )
        target_link_libraries(
            sranipatch-client
            PRIVATE user32 gdi32 comctl32 dwmapi uxtheme dxgi d3d11
        )
    endif()
    add_custom_command(
        TARGET sranipatch-client
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/head.glb"
            "$<TARGET_FILE_DIR:sranipatch-client>/head.glb"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Quicksand-Medium.ttf"
            "$<TARGET_FILE_DIR:sranipatch-client>/Quicksand-Medium.ttf"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-128.png"
            "$<TARGET_FILE_DIR:sranipatch-client>/icon-128.png"
    )
    install(TARGETS sranipatch-client RUNTIME DESTINATION bin COMPONENT Runtime)
    install(
        FILES
            assets/head.glb
            assets/fonts/Quicksand-Medium.ttf
            assets/icon-128.png
        DESTINATION bin
        COMPONENT Runtime
    )
    install(
        FILES assets/fonts/Quicksand-OFL.txt
        DESTINATION share/licenses
        COMPONENT Runtime
    )
endif()

if(SRANIPATCH_BUILD_DRIVER)
    add_library(driver_sranipatch MODULE src/driver/driver.cpp)
    target_include_directories(
        driver_sranipatch
        SYSTEM
        PRIVATE ${openvr_SOURCE_DIR}/headers
    )
    target_link_libraries(
        driver_sranipatch
        PRIVATE sranipatch_runtime ${CMAKE_DL_LIBS}
    )
    set_target_properties(driver_sranipatch PROPERTIES PREFIX "")
    sranipatch_target(driver_sranipatch)
    if(WIN32)
        set(sranipatch_driver_platform win64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(sranipatch_driver_platform linuxarm64)
    else()
        set(sranipatch_driver_platform linux64)
    endif()
    install(
        TARGETS driver_sranipatch
        RUNTIME
            DESTINATION driver/bin/${sranipatch_driver_platform}
            COMPONENT Runtime
        LIBRARY
            DESTINATION driver/bin/${sranipatch_driver_platform}
            COMPONENT Runtime
    )
    install(DIRECTORY driver/ DESTINATION driver COMPONENT Runtime)
    install(
        PROGRAMS scripts/register-steamvr.sh
        DESTINATION scripts
        COMPONENT Runtime
    )
    install(
        FILES scripts/register-steamvr.ps1
        DESTINATION scripts
        COMPONENT Runtime
    )
endif()

if(BUILD_TESTING)
    if(SRANIPATCH_BUILD_DRIVER)
        add_executable(sranipatch-driver-tests tests/driver_tests.cpp)
        target_include_directories(
            sranipatch-driver-tests
            SYSTEM
            PRIVATE ${openvr_SOURCE_DIR}/headers
        )
        target_link_libraries(
            sranipatch-driver-tests
            PRIVATE sranipatch_runtime ${CMAKE_DL_LIBS}
        )
        sranipatch_target(sranipatch-driver-tests)
        add_dependencies(sranipatch-driver-tests driver_sranipatch)
        add_test(
            NAME driver-lifecycle
            COMMAND sranipatch-driver-tests $<TARGET_FILE:driver_sranipatch>
        )
        set_tests_properties(driver-lifecycle PROPERTIES TIMEOUT 40)
    endif()
    add_executable(sranipatch-recovery-tests src/driver/recovery_tests.cpp)
    sranipatch_target(sranipatch-recovery-tests)
    add_test(NAME bounded-recovery COMMAND sranipatch-recovery-tests)
    add_executable(
        sranipatch-runtime-tests
        tests/runtime_tests.cpp
        src/server/runtime.cpp
    )
    target_include_directories(sranipatch-runtime-tests PRIVATE src/server)
    target_link_libraries(sranipatch-runtime-tests PRIVATE sranipatch_runtime)
    sranipatch_target(sranipatch-runtime-tests)
    add_test(NAME runtime-lifecycle COMMAND sranipatch-runtime-tests)
    set_tests_properties(runtime-lifecycle PROPERTIES TIMEOUT 30)
    add_executable(sranipatch-process-tests tests/process_tests.cpp)
    target_link_libraries(sranipatch-process-tests PRIVATE sranipatch_runtime)
    sranipatch_target(sranipatch-process-tests)
    add_test(NAME process-lifecycle COMMAND sranipatch-process-tests)
    set_tests_properties(process-lifecycle PROPERTIES TIMEOUT 30)
    add_executable(
        sranipatch-runtime-control-tests
        tests/runtime_control_tests.cpp
    )
    target_link_libraries(
        sranipatch-runtime-control-tests
        PRIVATE sranipatch_runtime
    )
    sranipatch_target(sranipatch-runtime-control-tests)
    add_test(NAME runtime-control COMMAND sranipatch-runtime-control-tests)
    if(SRANIPATCH_BUILD_CLIENT)
        add_executable(sranipatch-observation-tests tests/observation_tests.cpp)
        target_link_libraries(
            sranipatch-observation-tests
            PRIVATE sranipatch_observation
        )
        sranipatch_target(sranipatch-observation-tests)
        add_test(NAME client-observation COMMAND sranipatch-observation-tests)
        add_executable(sranipatch-head-tests tests/head_tests.cpp)
        target_link_libraries(
            sranipatch-head-tests
            PRIVATE sranipatch_head_model
        )
        sranipatch_target(sranipatch-head-tests)
        add_test(
            NAME head-model
            COMMAND
                sranipatch-head-tests
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/head.glb"
        )
        set_tests_properties(head-model PROPERTIES TIMEOUT 20)
        # Requires a graphics session; run explicitly rather than on headless CI.
        add_executable(
            sranipatch-head-render-tests
            tests/head_render_tests.cpp
            src/client/head_preview.cpp
        )
        target_link_libraries(
            sranipatch-head-render-tests
            PRIVATE sranipatch_head_model sranipatch_glad glfw
        )
        sranipatch_target(sranipatch-head-render-tests)
    endif()
    add_executable(sranipatch-tests tests/core_tests.cpp)
    target_link_libraries(sranipatch-tests PRIVATE sranipatch_core)
    sranipatch_target(sranipatch-tests)
    add_test(NAME core COMMAND sranipatch-tests)
    add_executable(sranipatch-config-reload-tests tests/config_reload_tests.cpp)
    target_link_libraries(
        sranipatch-config-reload-tests
        PRIVATE sranipatch_core
    )
    sranipatch_target(sranipatch-config-reload-tests)
    add_test(NAME config-reload COMMAND sranipatch-config-reload-tests)
    add_executable(
        sranipatch-tracking-tests
        tests/tracking_tests.cpp
        src/server/tracking.cpp
    )
    target_include_directories(sranipatch-tracking-tests PRIVATE src/server)
    target_link_libraries(sranipatch-tracking-tests PRIVATE sranipatch_core)
    sranipatch_target(sranipatch-tracking-tests)
    add_test(NAME tracking-lifecycle COMMAND sranipatch-tracking-tests)
    set_tests_properties(tracking-lifecycle PROPERTIES TIMEOUT 15)
    if(NOT WIN32)
        add_executable(
            sranipatch-osc-discovery-tests
            tests/osc_discovery_tests.cpp
            src/osc.cpp
        )
        target_include_directories(
            sranipatch-osc-discovery-tests
            PRIVATE include
        )
        target_link_libraries(
            sranipatch-osc-discovery-tests
            PRIVATE Threads::Threads
        )
        sranipatch_target(sranipatch-osc-discovery-tests)
        add_test(NAME osc-discovery COMMAND sranipatch-osc-discovery-tests)
        set_tests_properties(osc-discovery PROPERTIES TIMEOUT 30)
    endif()
    if(SRANIPATCH_BUILD_SERVER)
        add_executable(sranipatch-inference-tests tests/inference_tests.cpp)
        target_link_libraries(
            sranipatch-inference-tests
            PRIVATE sranipatch_inference MNN
        )
        sranipatch_target(sranipatch-inference-tests)
        add_test(
            NAME lip-inference
            COMMAND
                sranipatch-inference-tests
                "${CMAKE_CURRENT_SOURCE_DIR}/../assets/models/lip_model.mnn"
        )
        set_tests_properties(lip-inference PROPERTIES TIMEOUT 20)
    endif()
endif()

set(CPACK_PACKAGE_NAME "sranipatch")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP;TGZ")
set(CPACK_COMPONENTS_ALL Runtime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
if(
    MSVC
    AND (
        SRANIPATCH_BUILD_SERVER
        OR SRANIPATCH_BUILD_CLIENT
        OR SRANIPATCH_BUILD_DRIVER
    )
)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT Runtime)
    include(InstallRequiredSystemLibraries)
    if(SRANIPATCH_BUILD_DRIVER)
        install(
            FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
            DESTINATION driver/bin/${sranipatch_driver_platform}
            COMPONENT Runtime
        )
    endif()
endif()
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../output")
include(CPack)
