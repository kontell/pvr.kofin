cmake_minimum_required(VERSION 3.5...4.0)
project(pvr.kofin)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

# When cross-compiling (e.g., Android NDK), CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
# is set to ONLY which prevents find_package from searching CMAKE_PREFIX_PATH.
# Add the addon depends path to CMAKE_FIND_ROOT_PATH so Kodi can be found.
if(ADDON_DEPENDS_PATH)
  list(APPEND CMAKE_FIND_ROOT_PATH ${ADDON_DEPENDS_PATH})
endif()

find_package(Kodi REQUIRED)
find_package(JsonCpp REQUIRED)

include_directories(${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways
                    ${JSONCPP_INCLUDE_DIRS})

set(DEPLIBS ${JSONCPP_LIBRARIES})

message(STATUS "JSONCPP_LIBRARIES: ${JSONCPP_LIBRARIES}")

set(KOFIN_SOURCES src/addon.cpp
                  src/IptvSimple.cpp
                  src/iptvsimple/CatchupController.cpp
                  src/iptvsimple/Channels.cpp
                  src/iptvsimple/ChannelGroups.cpp
                  src/iptvsimple/ConnectionManager.cpp
                  src/iptvsimple/InstanceSettings.cpp
                  src/iptvsimple/M3UParser.cpp
                  src/iptvsimple/jellyfin/JellyfinClient.cpp
                  src/iptvsimple/jellyfin/JellyfinChannelLoader.cpp
                  src/iptvsimple/jellyfin/JellyfinRecordingManager.cpp
                  src/iptvsimple/jellyfin/JellyfinAuth.cpp
                  src/iptvsimple/data/Channel.cpp
                  src/iptvsimple/data/ChannelGroup.cpp
                  src/iptvsimple/utilities/FileUtils.cpp
                  src/iptvsimple/utilities/Logger.cpp
                  src/iptvsimple/utilities/StreamUtils.cpp
                  src/iptvsimple/utilities/Toast.cpp
                  src/iptvsimple/utilities/WebUtils.cpp
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
                  src/iptvsimple/jellyfin/JellyfinClient.h
                  src/iptvsimple/jellyfin/JellyfinChannelLoader.h
                  src/iptvsimple/jellyfin/JellyfinRecordingManager.h
                  src/iptvsimple/jellyfin/JellyfinAuth.h
                  src/iptvsimple/data/Channel.h
                  src/iptvsimple/data/ChannelGroup.h
                  src/iptvsimple/utilities/FileUtils.h
                  src/iptvsimple/utilities/JsonUtils.h
                  src/iptvsimple/utilities/Logger.h
                  src/iptvsimple/utilities/StreamUtils.h
                  src/iptvsimple/utilities/TimeUtils.h
                  src/iptvsimple/utilities/Toast.h
                  src/iptvsimple/utilities/UidUtils.h
                  src/iptvsimple/utilities/WebUtils.h
                  )

addon_version(pvr.kofin KOFIN)
add_definitions(-DKOFIN_VERSION=${KOFIN_VERSION})

build_addon(pvr.kofin KOFIN DEPLIBS)

include(CPack)
