cmake_minimum_required(VERSION 3.5)
project(pvr.kofin)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

find_package(Kodi REQUIRED)
find_package(JsonCpp REQUIRED)
find_package(pugixml REQUIRED)
find_package(ZLIB REQUIRED)
find_package(lzma REQUIRED)

include_directories(${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways
                    ${JSONCPP_INCLUDE_DIRS}
                    ${PUGIXML_INCLUDE_DIRS}
                    ${ZLIB_INCLUDE_DIRS}
                    ${LZMA_INCLUDE_DIRS})

set(DEPLIBS ${JSONCPP_LIBRARIES}
            ${PUGIXML_LIBRARIES}
            ${ZLIB_LIBRARIES}
            ${LZMA_LIBRARIES})

message(STATUS "JSONCPP_LIBRARIES: ${JSONCPP_LIBRARIES}")
message(STATUS "PUGIXML_LIBRARIES: ${PUGIXML_LIBRARIES}")
message(STATUS "ZLIB_LIBRARIES: ${ZLIB_LIBRARIES}")
message(STATUS "LZMA_LIBRARIES: ${LZMA_LIBRARIES}")

set(KOFIN_SOURCES src/addon.cpp
                  src/IptvSimple.cpp
                  src/iptvsimple/CatchupController.cpp
                  src/iptvsimple/Channels.cpp
                  src/iptvsimple/ChannelGroups.cpp
                  src/iptvsimple/ConnectionManager.cpp
                  src/iptvsimple/InstanceSettings.cpp
                  src/iptvsimple/M3UParser.cpp
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
                  src/iptvsimple/CatchupController.h
                  src/iptvsimple/Channels.h
                  src/iptvsimple/ChannelGroups.h
                  src/iptvsimple/ConnectionManager.h
                  src/iptvsimple/IConnectionListener.h
                  src/iptvsimple/InstanceSettings.h
                  src/iptvsimple/M3UParser.h
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

addon_version(pvr.kofin KOFIN)
add_definitions(-DKOFIN_VERSION=${KOFIN_VERSION})

# Detect PVR API version for v21/v22 compatibility
if(KODI_INCLUDE_DIR)
  if(EXISTS "${KODI_INCLUDE_DIR}/kodi/versions.h")
    set(_versions_h "${KODI_INCLUDE_DIR}/kodi/versions.h")
  elseif(EXISTS "${KODI_INCLUDE_DIR}/versions.h")
    set(_versions_h "${KODI_INCLUDE_DIR}/versions.h")
  endif()
  if(_versions_h)
    file(STRINGS "${_versions_h}" _pvr_ver_line
         REGEX "ADDON_INSTANCE_VERSION_PVR[^_]")
    if(_pvr_ver_line MATCHES "\"([0-9]+)\\.")
      set(KODI_PVR_API_MAJOR ${CMAKE_MATCH_1})
      message(STATUS "Detected PVR API major version: ${KODI_PVR_API_MAJOR}")
      if(KODI_PVR_API_MAJOR GREATER_EQUAL 9)
        add_definitions(-DKODI_PVR_API_V9)
        message(STATUS "PVR API v9+ detected — enabling v22 compatibility")
      endif()
    endif()
  endif()
endif()

build_addon(pvr.kofin KOFIN DEPLIBS)

include(CPack)
