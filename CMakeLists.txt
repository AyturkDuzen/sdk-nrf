#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Point to NCS root directory.
set(NRF_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE PATH "NCS root directory")

include(cmake/extensions.cmake)
include(cmake/version.cmake)
include(cmake/multi_image.cmake)

zephyr_include_directories(include)

add_subdirectory(ext)
add_subdirectory(lib)
add_subdirectory(samples)
add_subdirectory(subsys)
add_subdirectory(modules)
add_subdirectory(drivers)
add_subdirectory(tests)
