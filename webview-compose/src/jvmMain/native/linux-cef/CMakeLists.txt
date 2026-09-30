cmake_minimum_required(VERSION 3.21)
project(composewebview_cef LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

if(NOT CEF_ROOT)
    message(FATAL_ERROR "CEF_ROOT must point to an extracted CEF binary distribution")
endif()
if(NOT CEF_OUTPUT_DIR)
    message(FATAL_ERROR "CEF_OUTPUT_DIR must be set")
endif()

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/libcef_dll_wrapper")
ADD_LOGICAL_TARGET(cef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

set(JAVA_HOME "$ENV{JAVA_HOME}")
if(NOT JAVA_HOME)
    find_package(Java 17 REQUIRED COMPONENTS Development)
    get_filename_component(JAVAC_REALPATH "${Java_JAVAC_EXECUTABLE}" REALPATH)
    get_filename_component(JAVA_BIN_DIR "${JAVAC_REALPATH}" DIRECTORY)
    get_filename_component(JAVA_HOME "${JAVA_BIN_DIR}/.." REALPATH)
endif()

add_library(compose_cef_linux SHARED cef_bridge.cpp)
SET_LIBRARY_TARGET_PROPERTIES(compose_cef_linux)
set_target_properties(compose_cef_linux PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CEF_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)
target_include_directories(compose_cef_linux PRIVATE
    "${JAVA_HOME}/include"
    "${JAVA_HOME}/include/linux"
    "${CEF_ROOT}"
)
target_link_libraries(compose_cef_linux PRIVATE
    libcef_dll_wrapper
    cef_lib
    ${CEF_STANDARD_LIBS}
    PkgConfig::GTK3
)
set_target_properties(compose_cef_linux PROPERTIES OUTPUT_NAME compose_cef_linux)

add_executable(cef_subprocess cef_subprocess.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(cef_subprocess)
set_target_properties(cef_subprocess PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CEF_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)
target_include_directories(cef_subprocess PRIVATE "${CEF_ROOT}")
target_link_libraries(cef_subprocess PRIVATE
    libcef_dll_wrapper
    cef_lib
    ${CEF_STANDARD_LIBS}
)
