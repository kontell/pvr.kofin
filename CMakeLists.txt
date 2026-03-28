cmake_minimum_required(VERSION 3.5)
project(pvr.kofin)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

# Read version from VERSION file if it exists, otherwise fall back to addon_version
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" KOFIN_VERSION_FROM_FILE)
  string(STRIP "${KOFIN_VERSION_FROM_FILE}" KOFIN_VERSION_FROM_FILE)
  message(STATUS "Version from VERSION file: ${KOFIN_VERSION_FROM_FILE}")
endif()

set(KOFIN_SOURCES src/addon.cpp
                  src/IptvSimple.cpp
                  src/iptvsimple/Channels.cpp
                  src/iptvsimple/ChannelGroups.cpp
                  src/iptvsimple/ConnectionManager.cpp
                  src/iptvsimple/InstanceSettings.cpp
                  src/iptvsimple/Providers.cpp
                  src/iptvsimple/jellyfin/JellyfinClient.cpp
                  src/iptvsimple/jellyfin/JellyfinChannelLoader.cpp
                  src/iptvsimple/jellyfin/JellyfinRecordingManager.cpp
                  src/iptvsimple/data/Channel.cpp
                  src/iptvsimple/data/ChannelEpg.cpp
                  src/iptvsimple/data/ChannelGroup.cpp
                  src/iptvsimple/data/Provider.cpp
                  src/iptvsimple/data/EpgEntry.cpp
                  src/iptvsimple/data/EpgGenre.cpp
                  src/iptvsimple/utilities/FileUtils.cpp
                  src/iptvsimple/utilities/Logger.cpp
                  src/iptvsimple/utilities/SettingsMigration.cpp
                  src/iptvsimple/utilities/StreamUtils.cpp
                  src/iptvsimple/utilities/WebUtils.cpp
                  src/iptvsimple/utilities/WebStreamExtractor.cpp
                  src/iptvsimple/utilities/CurlUtils.cpp
                  )

set(KOFIN_HEADERS src/addon.h
                  src/IptvSimple.h
                  src/iptvsimple/Channels.h
                  src/iptvsimple/ChannelGroups.h
                  src/iptvsimple/ConnectionManager.h
                  src/iptvsimple/IConnectionListener.h
                  src/iptvsimple/InstanceSettings.h
                  src/iptvsimple/Providers.h
                  src/iptvsimple/jellyfin/JellyfinClient.h
                  src/iptvsimple/jellyfin/JellyfinChannelLoader.h
                  src/iptvsimple/jellyfin/JellyfinRecordingManager.h
                  src/iptvsimple/data/BaseEntry.h
                  src/iptvsimple/data/Channel.h
                  src/iptvsimple/data/ChannelEpg.h
                  src/iptvsimple/data/ChannelGroup.h
                  src/iptvsimple/data/Provider.h
                  src/iptvsimple/data/EpgEntry.h
                  src/iptvsimple/data/EpgGenre.h
                  src/iptvsimple/data/StreamEntry.h
                  src/iptvsimple/utilities/FileUtils.h
                  src/iptvsimple/utilities/Logger.h
                  src/iptvsimple/utilities/SettingsMigration.h
                  src/iptvsimple/utilities/StreamUtils.h
                  src/iptvsimple/utilities/TimeUtils.h
                  src/iptvsimple/utilities/WebUtils.h
                  src/iptvsimple/utilities/XMLUtils.h
                  src/iptvsimple/utilities/WebStreamExtractor.h
                  src/iptvsimple/utilities/CurlUtils.h
                  )

# Check if building standalone (e.g., Android cross-compilation)
if(DEFINED KODI_INCLUDE_DIR AND NOT KODI_FOUND)
  message(STATUS "Building in standalone mode with KODI_INCLUDE_DIR: ${KODI_INCLUDE_DIR}")
  set(STANDALONE_BUILD TRUE)

  # Version
  if(KOFIN_VERSION_FROM_FILE)
    set(KOFIN_VERSION ${KOFIN_VERSION_FROM_FILE})
  else()
    set(KOFIN_VERSION "0.1.0")
  endif()
  add_definitions(-DKOFIN_VERSION=${KOFIN_VERSION})

  # Detect PVR API version from Kodi headers
  if(KODI_INCLUDE_DIR)
    # Try both paths: standalone has include/ (needs kodi/ prefix), normal has kodi/ already
    if(EXISTS "${KODI_INCLUDE_DIR}/kodi/versions.h")
      set(_versions_h "${KODI_INCLUDE_DIR}/kodi/versions.h")
    else()
      set(_versions_h "${KODI_INCLUDE_DIR}/versions.h")
    endif()
    file(STRINGS "${_versions_h}" _pvr_ver_line
         REGEX "ADDON_INSTANCE_VERSION_PVR[^_]")
    if(_pvr_ver_line MATCHES "\"([0-9]+)\\.")
      set(KODI_PVR_API_MAJOR ${CMAKE_MATCH_1})
      message(STATUS "Detected PVR API major version: ${KODI_PVR_API_MAJOR}")
      if(KODI_PVR_API_MAJOR GREATER_EQUAL 9)
        add_definitions(-DKODI_PVR_API_V9)
      endif()
    endif()
  endif()

  # jsoncpp - check JSONCPP_ROOT, then fallback to Docker path
  if(DEFINED JSONCPP_ROOT)
    set(JSONCPP_LIB_PATH "${JSONCPP_ROOT}/lib/libjsoncpp.a")
    set(JSONCPP_INCLUDE "${JSONCPP_ROOT}/include")
  else()
    set(JSONCPP_LIB_PATH "/opt/jsoncpp-android/lib/libjsoncpp.a")
    set(JSONCPP_INCLUDE "/opt/jsoncpp-android/include")
  endif()

  include_directories(
    ${KODI_INCLUDE_DIR}
    ${KODI_INCLUDE_DIR}/..
    ${JSONCPP_INCLUDE}
    ${CMAKE_PREFIX_PATH}/include
  )

  add_library(pvr.kofin MODULE ${KOFIN_SOURCES})

  set_target_properties(pvr.kofin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pvr.kofin"
    SUFFIX ".so"
  )

  if(EXISTS ${JSONCPP_LIB_PATH})
    message(STATUS "jsoncpp found at: ${JSONCPP_LIB_PATH}")
    target_link_libraries(pvr.kofin ${JSONCPP_LIB_PATH})
  else()
    message(FATAL_ERROR "jsoncpp not found at ${JSONCPP_LIB_PATH}")
  endif()

  # pugixml, zlib, lzma from Kodi depends tree
  find_library(PUGIXML_LIB pugixml HINTS ${CMAKE_FIND_ROOT_PATH}/lib ${CMAKE_PREFIX_PATH}/lib NO_CMAKE_FIND_ROOT_PATH)
  find_library(ZLIB_LIB z HINTS ${CMAKE_FIND_ROOT_PATH}/lib ${CMAKE_PREFIX_PATH}/lib)
  find_library(LZMA_LIB lzma HINTS ${CMAKE_FIND_ROOT_PATH}/lib ${CMAKE_PREFIX_PATH}/lib NO_CMAKE_FIND_ROOT_PATH)

  if(PUGIXML_LIB)
    message(STATUS "pugixml: ${PUGIXML_LIB}")
    target_link_libraries(pvr.kofin ${PUGIXML_LIB})
  else()
    message(WARNING "pugixml not found in depends tree")
  endif()

  if(ZLIB_LIB)
    message(STATUS "zlib: ${ZLIB_LIB}")
    target_link_libraries(pvr.kofin ${ZLIB_LIB})
  else()
    message(WARNING "zlib not found in depends tree")
  endif()

  if(LZMA_LIB)
    message(STATUS "lzma: ${LZMA_LIB}")
    target_link_libraries(pvr.kofin ${LZMA_LIB})
  else()
    message(WARNING "lzma not found in depends tree")
  endif()

  if(ANDROID)
    target_link_libraries(pvr.kofin log)
  endif()

else()
  # Normal Kodi build system path (Linux, etc.)
  find_package(Kodi REQUIRED)
  find_package(pugixml REQUIRED)
  find_package(ZLIB REQUIRED)
  find_package(lzma REQUIRED)

  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(JSONCPP jsoncpp)
  endif()
  if(NOT JSONCPP_FOUND)
    find_package(JsonCpp QUIET)
  endif()
  if(NOT JSONCPP_FOUND)
    find_path(JSONCPP_INCLUDE_DIRS json/json.h)
    find_library(JSONCPP_LIBRARIES jsoncpp)
  endif()
  # Fallback: pkg-config may find the lib but omit -I for non-standard include paths
  if(NOT JSONCPP_INCLUDE_DIRS)
    find_path(JSONCPP_INCLUDE_DIRS json/json.h HINTS /usr/include/jsoncpp)
  endif()

  include_directories(${KODI_INCLUDE_DIR}/..
                      ${PUGIXML_INCLUDE_DIRS}
                      ${ZLIB_INCLUDE_DIRS}
                      ${LZMA_INCLUDE_DIRS}
                      ${JSONCPP_INCLUDE_DIRS})

  set(DEPLIBS ${PUGIXML_LIBRARIES}
              ${ZLIB_LIBRARIES}
              ${LZMA_LIBRARIES}
              ${JSONCPP_LIBRARIES})

  message(STATUS "PUGIXML_LIBRARIES: ${PUGIXML_LIBRARIES}")
  message(STATUS "ZLIB_LIBRARIES: ${ZLIB_LIBRARIES}")
  message(STATUS "LZMA_LIBRARIES: ${LZMA_LIBRARIES}")
  message(STATUS "JSONCPP_LIBRARIES: ${JSONCPP_LIBRARIES}")

  addon_version(pvr.kofin KOFIN)
  add_definitions(-DKOFIN_VERSION=${KOFIN_VERSION})

  # Detect PVR API version from Kodi headers
  if(KODI_INCLUDE_DIR)
    # Try both paths: standalone has include/ (needs kodi/ prefix), normal has kodi/ already
    if(EXISTS "${KODI_INCLUDE_DIR}/kodi/versions.h")
      set(_versions_h "${KODI_INCLUDE_DIR}/kodi/versions.h")
    else()
      set(_versions_h "${KODI_INCLUDE_DIR}/versions.h")
    endif()
    file(STRINGS "${_versions_h}" _pvr_ver_line
         REGEX "ADDON_INSTANCE_VERSION_PVR[^_]")
    if(_pvr_ver_line MATCHES "\"([0-9]+)\\.")
      set(KODI_PVR_API_MAJOR ${CMAKE_MATCH_1})
      message(STATUS "Detected PVR API major version: ${KODI_PVR_API_MAJOR}")
      if(KODI_PVR_API_MAJOR GREATER_EQUAL 9)
        add_definitions(-DKODI_PVR_API_V9)
      endif()
    endif()
  endif()

  if(WIN32)
    add_definitions(-DLZMA_API_STATIC)
  endif()

  build_addon(pvr.kofin KOFIN DEPLIBS)
endif()

include(CPack)
