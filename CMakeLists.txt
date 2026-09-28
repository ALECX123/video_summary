cmake_minimum_required(VERSION 3.16)
project(video_summary CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /MD (DLL) runtime everywhere: Qt 6.10.3 and the local OpenSSL build are both
# /MD. Must be set before any target is created (whisper.cpp included).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# --- whisper.cpp configuration (must be set BEFORE add_subdirectory) ---
set(BUILD_SHARED_LIBS OFF)
set(WHISPER_BUILD_TESTS OFF)
set(WHISPER_BUILD_EXAMPLES OFF)
set(WHISPER_BUILD_SERVER OFF)
set(GGML_CUDA ON)

add_subdirectory(third_party/whisper.cpp)

# Our binary lands in build/bin/video_summary.exe (multi-config safe).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Optional pieces - all gates degrade gracefully; none of them can break the
# CLI configure/build.
# ---------------------------------------------------------------------------

# --- openssl (needed only by bili_core / video_summary_gui) ---
if(NOT DEFINED OPENSSL_ROOT_DIR AND IS_DIRECTORY "F:/Code/openssl-3.0.22-win/lib")
    set(OPENSSL_ROOT_DIR "F:/Code/openssl-3.0.22-win" CACHE PATH "OpenSSL install root")
endif()
find_package(OpenSSL COMPONENTS SSL Crypto QUIET)
if(OpenSSL_FOUND OR OPENSSL_FOUND)
    set(VSUM_OPENSSL_ENABLED ON)
    message(STATUS "video_summary: openssl = ENABLED (root: ${OPENSSL_ROOT_DIR})")
else()
    set(VSUM_OPENSSL_ENABLED OFF)
    message(STATUS "video_summary: openssl = DISABLED (OpenSSL not found; bili_core / video_summary_gui will be skipped)")
endif()

# --- qt6 Widgets (needed only by video_summary_gui) ---
find_package(Qt6 COMPONENTS Widgets QUIET)
if(NOT Qt6_FOUND AND EXISTS "F:/Qt/qt6.10.3-static/lib/cmake/Qt6")
    list(APPEND CMAKE_PREFIX_PATH "F:/Qt/qt6.10.3-static")
    find_package(Qt6 COMPONENTS Widgets QUIET)
endif()
if(Qt6_FOUND)
    set(VSUM_QT6_ENABLED ON)
    message(STATUS "video_summary: qt6 = ENABLED (Qt6 ${Qt6_VERSION} Widgets)")
else()
    set(VSUM_QT6_ENABLED OFF)
    message(STATUS "video_summary: qt6 = DISABLED (Qt6 Widgets not found; video_summary_gui will be skipped)")
endif()

# --- gui sources (glob so they may appear at any time) ---
file(GLOB VSUM_GUI_CPP CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/gui/*.cpp")
file(GLOB VSUM_GUI_HDR CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/gui/*.h")
if(VSUM_GUI_CPP)
    set(VSUM_GUI_ENABLED ON)
    message(STATUS "video_summary: gui sources = ENABLED (${VSUM_GUI_CPP})")
else()
    set(VSUM_GUI_ENABLED OFF)
    message(STATUS "video_summary: gui sources = DISABLED (no gui/*.cpp present yet)")
endif()

# ---------------------------------------------------------------------------
# vsum_core - shared core (pipeline / audio / ollama), used by CLI and GUI.
# ---------------------------------------------------------------------------
add_library(vsum_core STATIC
    src/pipeline.cpp
    src/audio.cpp
    src/ollama.cpp
    src/pipeline.h
    src/audio.h
    src/ollama.h
    src/util.h
)

target_include_directories(vsum_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vsum_core PUBLIC whisper winhttp)

if(MSVC)
    # /utf-8: source files and string literals are UTF-8 (Chinese messages).
    target_compile_options(vsum_core PRIVATE /utf-8)
endif()

# ---------------------------------------------------------------------------
# video_summary - the CLI (entry point only + vsum_core).
# ---------------------------------------------------------------------------
add_executable(video_summary src/main.cpp)

target_link_libraries(video_summary PRIVATE vsum_core)

if(MSVC)
    target_compile_options(video_summary PRIVATE /utf-8)
endif()

# Multi-config generators (VS): DLLs land in <build>/bin/<Config> but the exe
# goes to <build>/bin - copy runtime DLLs next to the exe so it starts standalone.
# Single-config generators (Ninja): both already land in <build>/bin - no copy.
if(CMAKE_CONFIGURATION_TYPES)
    add_custom_command(TARGET video_summary POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                $<TARGET_FILE_DIR:whisper>
                $<TARGET_FILE_DIR:video_summary>
        COMMENT "Copying whisper/ggml runtime DLLs next to video_summary.exe"
    )
endif()

# ---------------------------------------------------------------------------
# bili_core - static lib for the GUI's bilibili downloader.
# Defined only when OpenSSL is found AND gui/BiliDownload.cpp exists AND
# third_party/qrcode/QrCode.cpp exists.
# ---------------------------------------------------------------------------
set(VSUM_BILI_ENABLED OFF)
set(VSUM_BILI_SKIP "")
if(NOT VSUM_OPENSSL_ENABLED)
    set(VSUM_BILI_SKIP "openssl not found")
elseif(NOT EXISTS "${CMAKE_SOURCE_DIR}/gui/BiliDownload.cpp")
    set(VSUM_BILI_SKIP "gui/BiliDownload.cpp does not exist yet")
elseif(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/qrcode/QrCode.cpp")
    set(VSUM_BILI_SKIP "third_party/qrcode/QrCode.cpp does not exist yet")
endif()

if(VSUM_BILI_SKIP STREQUAL "")
    set(VSUM_BILI_SOURCES
        "${CMAKE_SOURCE_DIR}/gui/BiliDownload.cpp"
        "${CMAKE_SOURCE_DIR}/third_party/qrcode/QrCode.cpp"
    )
    if(EXISTS "${CMAKE_SOURCE_DIR}/gui/BiliDownload.h")
        list(APPEND VSUM_BILI_SOURCES "${CMAKE_SOURCE_DIR}/gui/BiliDownload.h")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/qrcode/QrCode.h")
        list(APPEND VSUM_BILI_SOURCES "${CMAKE_SOURCE_DIR}/third_party/qrcode/QrCode.h")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/cpp-httplib/httplib.h")
        list(APPEND VSUM_BILI_SOURCES "${CMAKE_SOURCE_DIR}/third_party/cpp-httplib/httplib.h")
    endif()

    add_library(bili_core STATIC ${VSUM_BILI_SOURCES})

    target_include_directories(bili_core PUBLIC
        "${CMAKE_SOURCE_DIR}/gui"
        "${CMAKE_SOURCE_DIR}/third_party/cpp-httplib"
        "${CMAKE_SOURCE_DIR}/third_party/qrcode"
    )
    target_link_libraries(bili_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(bili_core PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    if(WIN32)
        target_link_libraries(bili_core PUBLIC ws2_32 crypt32)
    endif()
    if(MSVC)
        target_compile_options(bili_core PRIVATE /utf-8)
    endif()

    set(VSUM_BILI_ENABLED ON)
    message(STATUS "video_summary: bili_core = ENABLED (openssl + gui/BiliDownload.cpp + third_party/qrcode/QrCode.cpp)")
else()
    message(STATUS "video_summary: bili_core = DISABLED (${VSUM_BILI_SKIP})")
endif()

# ---------------------------------------------------------------------------
# video_summary_gui - WIN32 GUI app.
# Defined only when Qt6 Widgets found AND bili_core defined AND at least one
# gui/*.cpp exists.
# ---------------------------------------------------------------------------
set(VSUM_GUI_TARGET_ENABLED OFF)
set(VSUM_GUI_SKIP "")
if(NOT VSUM_QT6_ENABLED)
    set(VSUM_GUI_SKIP "Qt6 Widgets not found")
elseif(NOT VSUM_BILI_ENABLED)
    set(VSUM_GUI_SKIP "bili_core not defined")
elseif(NOT VSUM_GUI_CPP)
    set(VSUM_GUI_SKIP "no gui/*.cpp present")
endif()

if(VSUM_GUI_SKIP STREQUAL "")
    add_executable(video_summary_gui ${VSUM_GUI_CPP} ${VSUM_GUI_HDR})

    set_target_properties(video_summary_gui PROPERTIES
        AUTOMOC ON                 # per-target; CMAKE_AUTOMOC stays off globally
        WIN32_EXECUTABLE TRUE
    )

    target_link_libraries(video_summary_gui PRIVATE bili_core vsum_core Qt6::Widgets)

    if(MSVC)
        target_compile_options(video_summary_gui PRIVATE /utf-8)
    endif()

    # Ship OpenSSL runtime DLLs next to the exe.
    if(DEFINED OPENSSL_ROOT_DIR AND OPENSSL_ROOT_DIR)
        set(VSUM_SSL_BIN_DIR "${OPENSSL_ROOT_DIR}/bin")
    else()
        set(VSUM_SSL_BIN_DIR "F:/Code/openssl-3.0.22-win/bin")
    endif()
    foreach(_vsum_ssl_dll libssl-3-x64.dll libcrypto-3-x64.dll)
        if(EXISTS "${VSUM_SSL_BIN_DIR}/${_vsum_ssl_dll}")
            add_custom_command(TARGET video_summary_gui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${VSUM_SSL_BIN_DIR}/${_vsum_ssl_dll}"
                        "$<TARGET_FILE_DIR:video_summary_gui>"
                COMMENT "Copying ${_vsum_ssl_dll} next to video_summary_gui.exe"
            )
        else()
            message(STATUS "video_summary: ${VSUM_SSL_BIN_DIR}/${_vsum_ssl_dll} not found - copy skipped")
        endif()
    endforeach()

    # Deploy Qt runtime next to the exe.
    find_program(VSUM_WINDEPLOYQT windeployqt HINTS "F:/Qt/qt6.10.3-static/bin")
    if(VSUM_WINDEPLOYQT)
        add_custom_command(TARGET video_summary_gui POST_BUILD
                COMMAND "${VSUM_WINDEPLOYQT}" --no-translations "$<TARGET_FILE:video_summary_gui>"
                COMMENT "Running windeployqt on video_summary_gui.exe"
        )
    else()
        message(STATUS "video_summary: windeployqt not found - Qt runtime deploy skipped")
    endif()

    set(VSUM_GUI_TARGET_ENABLED ON)
    message(STATUS "video_summary: video_summary_gui = ENABLED (qt6 + bili_core + gui sources)")
else()
    message(STATUS "video_summary: video_summary_gui = DISABLED (${VSUM_GUI_SKIP})")
endif()
