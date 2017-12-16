################################################################################
#
# - Try to find libs3 headers and libraries.
#
# Usage of this module as follows:
#
#     find_package(LibS3)
#
# Variables used by this module, they can change the default behaviour and need
# to be set before calling find_package:
#
#  LIBS3_ROOT_DIR Set this variable to the root installation of
#                 libs3 if the module has problems finding
#                 the proper installation path.
#
# Variables defined by this module:
#
#  LIBS3_FOUND             System has libs3 libs/headers
#  LIBS3_LIBRARIES         The libs3 library/libraries
#  LIBS3_INCLUDE_DIRS      The location of libs3 headers

find_path(LIBS3_ROOT_DIR
  NAMES include/libs3.h)

find_library(LIBS3_LIBRARIES
  NAMES s3
  HINTS ${LIBS3_ROOT_DIR}/lib)

find_path(LIBS3_INCLUDE_DIRS
  NAMES libs3.h
  HINTS ${LIBS3_ROOT_DIR}/include)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LIBS3
  DEFAULT_MSG LIBS3_LIBRARIES LIBS3_INCLUDE_DIRS)

mark_as_advanced(LIBS3_ROOT_DIR)
mark_as_advanced(LIBS3_LIBRARIES)
mark_as_advanced(LIBS3_INCLUDE_DIRS)
