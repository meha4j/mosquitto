# This is a cmake script. Process it with the CMake gui or command line utility
# to produce makefiles / Visual Studio project files on Mac OS X and Windows.
#
# To configure the build options either use the CMake gui, or run the command
# line utility including the "-i" option.

cmake_minimum_required(VERSION 3.5)
cmake_policy(SET CMP0042 NEW)

set(CMAKE_TOOLCHAIN_FILE "vcpkg/scripts/buildsystems/vcpkg.cmake")
set(CMAKE_EXPORT_COMPILE_COMMAND true)

project(mosquitto)
set(VERSION 2.0.19)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

add_definitions(-DCMAKE -DVERSION=\"${VERSION}\")

if(WIN32)
  add_definitions("-D_CRT_SECURE_NO_WARNINGS")
  add_definitions("-D_CRT_NONSTDC_NO_DEPRECATE")
endif(WIN32)

if(APPLE)
  set(CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS
      "${CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS} -undefined dynamic_lookup")
endif(APPLE)

include(GNUInstallDirs)

option(WITH_THREADING "Include client library threading support?" ON)

option(WITH_BUNDLED_DEPS "Build with bundled dependencies?" OFF)
option(WITH_TLS "Include SSL/TLS support?" OFF)
option(WITH_TLS_PSK "Include TLS-PSK support (requires WITH_TLS)?" OFF)
option(WITH_EC "Include Elliptic Curve support (requires WITH_TLS)?" OFF)
option(WITH_UNIX_SOCKETS "Include Unix Domain Socket support?" OFF)
option(WITH_SOCKS "Include SOCKS5 support?" OFF)
option(WITH_SRV "Include SRV lookup support?" OFF)
option(WITH_STATIC_LIBRARIES "Build static libraries?" OFF)
option(WITH_PIC "Build the static library with PIC enabled archives?" OFF)
option(WITH_DLT "Include DLT support?" OFF)

find_package(cJSON REQUIRED)
find_path(UTHASH_INCLUDE_DIRS "uthash.h")

if(WITH_TLS)
  find_package(OpenSSL REQUIRED)
  add_definitions("-DWITH_TLS")

  # mosquitto uses OpenSSL 1.1 API, so set OPENSSL_API_COMPAT accordingly:
  # https://www.openssl.org/docs/manmaster/man7/OPENSSL_API_COMPAT.html TODO:
  # migrate off ENGINE API (deprecated since OpenSSL 3.0), see:
  # https://www.openssl.org/docs/manmaster/man7/migration_guide.html#Engines-and-METHOD-APIs
  add_definitions("-DOPENSSL_API_COMPAT=0x10100000L")

  if(WITH_TLS_PSK)
    add_definitions("-DWITH_TLS_PSK")
  endif(WITH_TLS_PSK)

  if(WITH_EC)
    add_definitions("-DWITH_EC")
  endif(WITH_EC)
else(WITH_TLS)
  set(OPENSSL_INCLUDE_DIR "")
endif(WITH_TLS)

if(WITH_UNIX_SOCKETS AND NOT WIN32)
  add_definitions("-DWITH_UNIX_SOCKETS")
endif(WITH_UNIX_SOCKETS AND NOT WIN32)

if(WITH_SOCKS)
  add_definitions("-DWITH_SOCKS")
endif(WITH_SOCKS)

if(WITH_THREADING)
  add_definitions("-DWITH_THREADING")

  if(WIN32)
    find_package(Pthreads4W REQUIRED)
  endif()
endif(WITH_THREADING)

if(WITH_DLT)
  find_package(PkgConfig)
  pkg_check_modules(DLT "automotive-dlt >= 2.11")
  add_definitions("-DWITH_DLT")
endif(WITH_DLT)

# ========================================
# Include projects
# ========================================

option(WITH_LEGACY "Include CAS protocol support?" ON)
option(WITH_CLIENTS "Build clients?" ON)
option(WITH_BROKER "Build broker?" ON)
option(WITH_APPS "Build apps?" ON)
option(WITH_PLUGINS "Build plugins?" ON)
option(DOCUMENTATION "Build documentation?" OFF)

add_subdirectory(lib)

if(WITH_LEGACY)
  add_subdirectory(legacy)
endif(WITH_LEGACY)

if(WITH_CLIENTS)
  add_subdirectory(client)
endif(WITH_CLIENTS)

if(WITH_BROKER)
  add_subdirectory(src)
endif(WITH_BROKER)

if(WITH_APPS)
  add_subdirectory(apps)
endif(WITH_APPS)

if(WITH_PLUGINS)
  add_subdirectory(plugins)
endif(WITH_PLUGINS)

if(DOCUMENTATION)
  add_subdirectory(man)
endif(DOCUMENTATION)

# ========================================
# Install config file
# ========================================

if(WITH_BROKER)
  install(FILES mosquitto.conf aclfile.example pskfile.example pwfile.example
          DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/mosquitto")
endif(WITH_BROKER)

# ========================================
# Install pkg-config files
# ========================================

configure_file(libmosquitto.pc.in libmosquitto.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libmosquitto.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
configure_file(libmosquittopp.pc.in libmosquittopp.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libmosquittopp.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# ========================================
# Testing
# ========================================
enable_testing()
