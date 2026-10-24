cmake_minimum_required(VERSION 3.25)
project(sranipal_on_frame VERSION 0.1.0 LANGUAGES C CXX)

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

option(SOF_BUILD_SERVER "Build camera / MNN / OSC server" ON)
option(SOF_BUILD_CLIENT "Build Dear ImGui desktop client" ON)
option(
    SOF_ENABLE_IPO
    "Enable release link-time optimization for application code"
    ON
)
option(SOF_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
include(CTest)
include(cmake/Dependencies.cmake)
find_package(Threads REQUIRED)

add_library(sof_options INTERFACE)
target_compile_features(sof_options INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(
        sof_options
        INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus
    )
    target_compile_definitions(
        sof_options
        INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN
    )
else()
    target_compile_options(
        sof_options
        INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_link_options(sof_options INTERFACE -Wl,-z,noexecstack)
    if(SOF_SANITIZE)
        target_compile_options(
            sof_options
            INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(sof_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

if(SOF_ENABLE_IPO AND NOT SOF_SANITIZE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT sof_ipo_supported OUTPUT sof_ipo_error)
endif()
function(sof_target target)
    target_link_libraries(${target} PRIVATE sof_options)
    if(sof_ipo_supported)
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
    sof_core
    STATIC
    src/ipc.cpp
    src/osc.cpp
    src/frame.cpp
    src/config.cpp
    src/capture/vive_tracker.cpp
)
target_include_directories(sof_core PUBLIC include)
target_link_libraries(sof_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(sof_core PRIVATE ws2_32)
endif()
sof_target(sof_core)

if(SOF_BUILD_SERVER)
    sof_add_mnn()
    sof_add_opencv()
    if(WIN32)
        set(capture_source src/capture/media_foundation.cpp)
    else()
        set(capture_source src/capture/v4l2.cpp)
    endif()
    add_library(sof_capture STATIC ${capture_source})
    target_link_libraries(sof_capture PUBLIC sof_core)
    sof_target(sof_capture)
    add_library(
        sof_inference
        STATIC
        src/inference/mnn.cpp
        src/inference/lip.cpp
        src/inference/image.cpp
    )
    target_include_directories(
        sof_inference
        SYSTEM
        PRIVATE "${mnn_SOURCE_DIR}/include"
    )
    target_include_directories(
        sof_inference
        SYSTEM
        PRIVATE ${sof_opencv_includes}
    )
    target_link_libraries(sof_inference PUBLIC sof_core PRIVATE MNN opencv_core)
    sof_target(sof_inference)
    add_executable(sof-server src/server/main.cpp)
    target_link_libraries(sof-server PRIVATE sof_capture sof_inference MNN)
    if(WIN32)
        target_link_libraries(
            sof_capture
            PRIVATE mfplat mfreadwrite mfuuid mf ole32 strmiids ksuser cfgmgr32
        )
    endif()
    sof_target(sof-server)
    install(TARGETS sof-server RUNTIME DESTINATION bin COMPONENT Runtime)
    install(
        FILES ../assets/models/lip_model.mnn
        DESTINATION share/sranipal-on-frame/models
        COMPONENT Runtime
    )
    install(
        FILES third_party/vivefacialtracker-LICENSE.txt
        DESTINATION share/licenses
        COMPONENT Runtime
    )
endif()

if(SOF_BUILD_CLIENT)
    sof_add_gui()
    add_library(sof_head_model STATIC src/client/head_model.cpp)
    target_include_directories(sof_head_model PUBLIC src/client)
    target_link_libraries(sof_head_model PUBLIC sof_core PRIVATE sof_cgltf)
    sof_target(sof_head_model)
    add_executable(sof-client src/client/main.cpp src/client/head_preview.cpp)
    target_link_libraries(sof-client PRIVATE sof_core sof_imgui sof_head_model)
    sof_target(sof-client)
    add_custom_command(
        TARGET sof-client
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/head.glb"
            "$<TARGET_FILE_DIR:sof-client>/head.glb"
    )
    install(TARGETS sof-client RUNTIME DESTINATION bin COMPONENT Runtime)
    install(FILES head.glb DESTINATION bin COMPONENT Runtime)
endif()

if(BUILD_TESTING)
    if(SOF_BUILD_CLIENT)
        add_executable(sof-head-tests tests/head_tests.cpp)
        target_link_libraries(sof-head-tests PRIVATE sof_head_model)
        sof_target(sof-head-tests)
        add_test(
            NAME head-model
            COMMAND sof-head-tests "${CMAKE_CURRENT_SOURCE_DIR}/head.glb"
        )
        set_tests_properties(head-model PROPERTIES TIMEOUT 20)
        # Requires a graphics session; run explicitly rather than on headless CI.
        add_executable(
            sof-head-render-tests
            tests/head_render_tests.cpp
            src/client/head_preview.cpp
        )
        target_link_libraries(
            sof-head-render-tests
            PRIVATE sof_head_model sof_imgui
        )
        sof_target(sof-head-render-tests)
    endif()
    add_executable(sof-tests tests/core_tests.cpp)
    target_link_libraries(sof-tests PRIVATE sof_core)
    sof_target(sof-tests)
    add_test(NAME core COMMAND sof-tests)
    if(SOF_BUILD_SERVER)
        add_executable(sof-inference-tests tests/inference_tests.cpp)
        target_link_libraries(sof-inference-tests PRIVATE sof_inference MNN)
        sof_target(sof-inference-tests)
        add_test(
            NAME lip-inference
            COMMAND
                sof-inference-tests
                "${CMAKE_CURRENT_SOURCE_DIR}/../assets/models/lip_model.mnn"
        )
        set_tests_properties(lip-inference PROPERTIES TIMEOUT 20)
    endif()
endif()

set(CPACK_PACKAGE_NAME "sranipal-on-frame")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP;TGZ")
set(CPACK_COMPONENTS_ALL Runtime)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
if(MSVC AND (SOF_BUILD_SERVER OR SOF_BUILD_CLIENT))
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT Runtime)
    include(InstallRequiredSystemLibraries)
endif()
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../output")
include(CPack)
