include(FetchContent)

# Immutable revisions; FetchContent source overrides also allow offline/vendor builds.
function(sof_add_mnn)
    set(MNN_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(MNN_WIN_RUNTIME_MT OFF CACHE BOOL "" FORCE)
    set(MNN_SEP_BUILD OFF CACHE BOOL "" FORCE)
    set(MNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(MNN_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(MNN_BUILD_CONVERTER OFF CACHE BOOL "" FORCE)
    set(MNN_BUILD_TRAIN OFF CACHE BOOL "" FORCE)
    set(MNN_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(MNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    # CPU backend: portable SIMD on x64 and NEON on AArch64. GPU backends are opt-in.
    FetchContent_Declare(
        mnn
        GIT_REPOSITORY https://github.com/alibaba/MNN.git
        GIT_TAG ebdada82634300956e08bd4056ecfeb1e4f23b32
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(mnn)
    if(MSVC)
        target_compile_options(MNNCore PRIVATE /w)
        target_compile_options(MNNTransform PRIVATE /w)
    endif()
    install(
        FILES ${mnn_SOURCE_DIR}/LICENSE.txt
        DESTINATION share/licenses
        RENAME MNN.txt
        COMPONENT Runtime
    )
    install(
        FILES ${mnn_SOURCE_DIR}/3rd_party/flatbuffers/LICENSE.txt
        DESTINATION share/licenses
        RENAME flatbuffers.txt
        COMPONENT Runtime
    )
    install(
        FILES ${mnn_SOURCE_DIR}/3rd_party/half/LICENSE.txt
        DESTINATION share/licenses
        RENAME half.txt
        COMPONENT Runtime
    )
    set(mnn_SOURCE_DIR "${mnn_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(sof_add_opencv)
    set(sof_executable_output_path "${EXECUTABLE_OUTPUT_PATH}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_LIST core,imgproc CACHE STRING "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_PERF_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_opencv_apps OFF CACHE BOOL "" FORCE)
    set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
    set(BUILD_opencv_python3 OFF CACHE BOOL "" FORCE)
    set(WITH_IPP OFF CACHE BOOL "" FORCE)
    set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
    set(WITH_ITT OFF CACHE BOOL "" FORCE)
    foreach(
        feature
        AVIF
        FFMPEG
        GSTREAMER
        GTK
        JPEG
        LAPACK
        OPENEXR
        OPENJPEG
        PNG
        TIFF
        UNIFONT
        V4L
        WEBP
        WIN32UI
    )
        set(WITH_${feature} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(BUILD_WITH_STATIC_CRT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        opencv
        GIT_REPOSITORY https://github.com/opencv/opencv.git
        GIT_TAG 40738fb16ceddb5fb3fea747585f7ce6abb0605b # 5.0.0
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(opencv)
    if(sof_executable_output_path)
        set(EXECUTABLE_OUTPUT_PATH
            "${sof_executable_output_path}"
            CACHE PATH
            ""
            FORCE
        )
    else()
        unset(EXECUTABLE_OUTPUT_PATH CACHE)
    endif()
    set_property(
        DIRECTORY "${opencv_SOURCE_DIR}"
        PROPERTY EXCLUDE_FROM_ALL TRUE
    )
    set(sof_opencv_includes
        "${opencv_SOURCE_DIR}/modules/core/include"
        "${opencv_SOURCE_DIR}/modules/imgproc/include"
        "${CMAKE_BINARY_DIR}"
        PARENT_SCOPE
    )
    install(
        FILES ${opencv_SOURCE_DIR}/LICENSE
        DESTINATION share/licenses
        RENAME OpenCV.txt
        COMPONENT Runtime
    )
    install(
        FILES ${opencv_SOURCE_DIR}/3rdparty/zlib/LICENSE
        DESTINATION share/licenses
        RENAME zlib.txt
        COMPONENT Runtime
    )
endfunction()

function(sof_add_gui)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Build both desktop backends. GLFW selects from the session environment;
        # scripts/run-linux.sh explicitly selects XWayland for affected Gamescope builds.
        option(GLFW_BUILD_WAYLAND "Build the GLFW Wayland backend" ON)
        option(GLFW_BUILD_X11 "Build the GLFW X11 backend" ON)
    endif()
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG a74efa0d5628b74adc0426af4c5710e287fa7c2c
    )
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG f5befd2d29e66809cd1110a152e375a7f1981f06
    )
    FetchContent_MakeAvailable(glfw imgui)
    find_package(OpenGL REQUIRED)
    add_library(
        sof_imgui
        STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(
        sof_imgui
        SYSTEM
        PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(sof_imgui PUBLIC glfw OpenGL::GL)
    install(
        FILES ${imgui_SOURCE_DIR}/LICENSE.txt
        DESTINATION share/licenses
        RENAME imgui.txt
        COMPONENT Runtime
    )
    install(
        FILES ${glfw_SOURCE_DIR}/LICENSE.md
        DESTINATION share/licenses
        RENAME glfw.txt
        COMPONENT Runtime
    )
endfunction()
