cmake_minimum_required(VERSION 3.25)
project(sranipatch VERSION 0.1.0 LANGUAGES C CXX)

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
option(SRANIPATCH_BUILD_CLIENT "Build Dear ImGui desktop client" ON)
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

add_library(
    sranipatch_core
    STATIC
    src/ipc.cpp
    src/osc.cpp
    src/frame.cpp
    src/config.cpp
    src/capture/vive_tracker.cpp
)
target_include_directories(sranipatch_core PUBLIC include)
target_link_libraries(sranipatch_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(sranipatch_core PRIVATE ws2_32)
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
    add_executable(sranipatch-server src/server/main.cpp)
    target_link_libraries(
        sranipatch-server
        PRIVATE sranipatch_capture sranipatch_inference MNN
    )
    if(WIN32)
        target_link_libraries(
            sranipatch_capture
            PRIVATE mfplat mfreadwrite mfuuid mf ole32 strmiids ksuser cfgmgr32
        )
    endif()
    sranipatch_target(sranipatch-server)
    install(TARGETS sranipatch-server RUNTIME DESTINATION bin COMPONENT Runtime)
    install(
        FILES ../assets/models/lip_model.mnn
        DESTINATION share/sranipatch/models
        COMPONENT Runtime
    )
    install(
        FILES third_party/vivefacialtracker-LICENSE.txt
        DESTINATION share/licenses
        COMPONENT Runtime
    )
endif()

if(SRANIPATCH_BUILD_CLIENT)
    sranipatch_add_gui()
    add_library(sranipatch_head_model STATIC src/client/head_model.cpp)
    target_include_directories(sranipatch_head_model PUBLIC src/client)
    target_link_libraries(
        sranipatch_head_model
        PUBLIC sranipatch_core
        PRIVATE sranipatch_cgltf
    )
    sranipatch_target(sranipatch_head_model)
    add_executable(
        sranipatch-client
        src/client/main.cpp
        src/client/head_preview.cpp
        src/client/style.cpp
        src/client/parameter_grid.cpp
    )
    target_link_libraries(
        sranipatch-client
        PRIVATE sranipatch_core sranipatch_imgui sranipatch_head_model
    )
    sranipatch_target(sranipatch-client)
    add_custom_command(
        TARGET sranipatch-client
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/head.glb"
            "$<TARGET_FILE_DIR:sranipatch-client>/head.glb"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Quicksand-Medium.ttf"
            "$<TARGET_FILE_DIR:sranipatch-client>/Quicksand-Medium.ttf"
    )
    install(TARGETS sranipatch-client RUNTIME DESTINATION bin COMPONENT Runtime)
    install(
        FILES head.glb assets/fonts/Quicksand-Medium.ttf
        DESTINATION bin
        COMPONENT Runtime
    )
    install(
        FILES assets/fonts/Quicksand-OFL.txt
        DESTINATION share/licenses
        COMPONENT Runtime
    )
endif()

if(BUILD_TESTING)
    if(SRANIPATCH_BUILD_CLIENT)
        add_executable(sranipatch-head-tests tests/head_tests.cpp)
        target_link_libraries(
            sranipatch-head-tests
            PRIVATE sranipatch_head_model
        )
        sranipatch_target(sranipatch-head-tests)
        add_test(
            NAME head-model
            COMMAND sranipatch-head-tests "${CMAKE_CURRENT_SOURCE_DIR}/head.glb"
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
            PRIVATE sranipatch_head_model sranipatch_imgui
        )
        sranipatch_target(sranipatch-head-render-tests)
    endif()
    add_executable(sranipatch-tests tests/core_tests.cpp)
    target_link_libraries(sranipatch-tests PRIVATE sranipatch_core)
    sranipatch_target(sranipatch-tests)
    add_test(NAME core COMMAND sranipatch-tests)
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
if(MSVC AND (SRANIPATCH_BUILD_SERVER OR SRANIPATCH_BUILD_CLIENT))
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT Runtime)
    include(InstallRequiredSystemLibraries)
endif()
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../output")
include(CPack)
