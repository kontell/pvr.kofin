cmake_minimum_required(VERSION 3.5)
project(pvr.kofin)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

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

include_directories(${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways
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

set(KOFIN_SOURCES src/addon.cpp
                  src/IptvSimple.cpp
                  src/iptvsimple/AddonSettings.cpp
                  src/iptvsimple/Channels.cpp
                  src/iptvsimple/ChannelGroups.cpp
                  src/iptvsimple/ConnectionManager.cpp
                  src/iptvsimple/InstanceSettings.cpp
                  src/iptvsimple/Providers.cpp
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
                  src/iptvsimple/AddonSettings.h
                  src/iptvsimple/Channels.h
                  src/iptvsimple/ChannelGroups.h
                  src/iptvsimple/ConnectionManager.h
                  src/iptvsimple/IConnectionListener.h
                  src/iptvsimple/InstanceSettings.h
                  src/iptvsimple/Providers.h
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

if(WIN32)
  add_definitions(-DLZMA_API_STATIC)
endif()

build_addon(pvr.kofin KOFIN DEPLIBS)

include(CPack)
