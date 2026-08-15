cmake_minimum_required(VERSION 3.16)
project(twinhost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "twinhost targets Windows only")
endif()

set(TWIN_SOURCES
    src/main.cpp
    src/util/log.cpp
    src/net/server.cpp
    src/capture/capture.cpp
    src/audio/capture.cpp
    src/audio/aac.cpp
    src/encode/encoder.cpp
    src/encode/nvenc.cpp
    src/encode/color.cpp
    src/encode/nal.cpp
    src/input/inject.cpp
)

set(TWIN_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
                      ${CMAKE_CURRENT_SOURCE_DIR}/../proto
                      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvenc)

# QSV encoder: compiles against the oneVPL C headers. They must be fetched
# once (see third_party/qsv/fetch.bat); the runtime DLL is loaded dynamically.
set(TWIN_HAVE_QSV 0)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qsv/api/vpl/mfx.h")
    set(TWIN_HAVE_QSV 1)
    list(APPEND TWIN_SOURCES src/encode/qsv.cpp)
    list(APPEND TWIN_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qsv/api/vpl)
    message(STATUS "QSV headers found; QSV encoder enabled")
else()
    message(WARNING "QSV headers not found; QSV encoder disabled (run host/third_party/qsv/fetch.bat)")
endif()

# AMF encoder: compiles against the AMF headers. They must be fetched once
# (see third_party/amf/fetch.bat); the runtime DLL is loaded dynamically.
set(TWIN_HAVE_AMF 0)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf/include/components/VideoEncoderVCE.h")
    set(TWIN_HAVE_AMF 1)
    list(APPEND TWIN_SOURCES src/encode/amf.cpp)
    list(APPEND TWIN_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/amf/include)
    message(STATUS "AMF headers found; AMF encoder enabled")
else()
    message(WARNING "AMF headers not found; AMF encoder disabled (run host/third_party/amf/fetch.bat)")
endif()

add_executable(twinhost ${TWIN_SOURCES})

target_include_directories(twinhost PRIVATE ${TWIN_INCLUDE_DIRS})

target_compile_definitions(twinhost PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00   # Windows 10
)
if(TWIN_HAVE_QSV)
    target_compile_definitions(twinhost PRIVATE TWIN_HAVE_QSV=1)
endif()
if(TWIN_HAVE_AMF)
    target_compile_definitions(twinhost PRIVATE TWIN_HAVE_AMF=1)
endif()

target_link_libraries(twinhost PRIVATE
    d3d11 dxgi dxguid d3d12 d3dcompiler ws2_32
    ole32 mfplat mf mfuuid uuid
)

if(MSVC)
    target_compile_options(twinhost PRIVATE /W4 /permissive-)
endif()
