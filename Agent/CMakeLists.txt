cmake_minimum_required(VERSION 3.20)
project(DeviceAgent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(cpp_httplib nlohmann_json)

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBSERIALPORT QUIET libserialport)
endif()

find_path(
  LIBSERIALPORT_INCLUDE_DIR
  NAMES serialport.h
  HINTS ${PC_LIBSERIALPORT_INCLUDE_DIRS}
)
find_library(
  LIBSERIALPORT_LIBRARY
  NAMES serialport libserialport
  HINTS ${PC_LIBSERIALPORT_LIBRARY_DIRS}
)

if(NOT LIBSERIALPORT_INCLUDE_DIR OR NOT LIBSERIALPORT_LIBRARY)
  message(FATAL_ERROR "libserialport not found. Install libserialport dev package.")
endif()

add_library(serialport::serialport UNKNOWN IMPORTED)
set_target_properties(
  serialport::serialport
  PROPERTIES
    IMPORTED_LOCATION "${LIBSERIALPORT_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBSERIALPORT_INCLUDE_DIR}"
)

set(SOURCES
  src/main.cpp
  src/api/HttpServer.cpp
  src/api/WebSocketServer.cpp
  src/serial/SerialManager.cpp
  src/serial/PortEnumerator.cpp
  src/core/EventBus.cpp
  src/core/SessionManager.cpp
  src/core/Backoff.cpp
)

add_executable(device_agent ${SOURCES})
target_include_directories(device_agent PRIVATE src)
target_link_libraries(
  device_agent
  PRIVATE
    Boost::system
    Boost::thread
    httplib::httplib
    nlohmann_json::nlohmann_json
    serialport::serialport
)

if(WIN32)
  target_compile_definitions(device_agent PRIVATE _WIN32_WINNT=0x0601)
  target_link_libraries(device_agent PRIVATE ws2_32)
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(device_agent PRIVATE pthread)
endif()

install(TARGETS device_agent RUNTIME DESTINATION bin)
