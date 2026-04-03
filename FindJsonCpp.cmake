# - Try to find jsoncpp
# Once done this will define
#
# JSONCPP_FOUND - system has jsoncpp
# JSONCPP_INCLUDE_DIRS - the jsoncpp include directory
# JSONCPP_LIBRARIES - The jsoncpp libraries

find_package(PkgConfig)

if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_JSONCPP jsoncpp QUIET)
endif()

find_path(JSONCPP_INCLUDE_DIRS json/json.h
          PATHS ${PC_JSONCPP_INCLUDEDIR}
          PATH_SUFFIXES jsoncpp)

find_library(JSONCPP_LIBRARIES jsoncpp
             PATHS ${PC_JSONCPP_LIBDIR})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(JsonCpp DEFAULT_MSG JSONCPP_INCLUDE_DIRS JSONCPP_LIBRARIES)

mark_as_advanced(JSONCPP_INCLUDE_DIRS JSONCPP_LIBRARIES)
