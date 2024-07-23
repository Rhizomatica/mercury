# Copyright 2011,2012,2016 Free Software Foundation, Inc.
#
# This file is part of GNU Radio
#
# GNU Radio is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# GNU Radio is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Radio; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.

########################################################################
# Setup library
########################################################################
include(GrPlatform) #define LIB_SUFFIX

include_directories(${Boost_INCLUDE_DIR})
link_directories(${Boost_LIBRARY_DIRS})

list(APPEND hermes_ofdm_sources
    zero_padder_impl.cc
    zero_depadder_impl.cc
    gi_adder_impl.cc
    gi_remover_impl.cc
    framer_impl.cc
    deframer_impl.cc
)

set(hermes_ofdm_sources "${hermes_ofdm_sources}" PARENT_SCOPE)
if(NOT hermes_ofdm_sources)
	MESSAGE(STATUS "No C++ sources... skipping lib/")
	return()
endif(NOT hermes_ofdm_sources)

add_library(gnuradio-hermes_ofdm SHARED ${hermes_ofdm_sources})
target_link_libraries(gnuradio-hermes_ofdm ${Boost_LIBRARIES} ${GNURADIO_ALL_LIBRARIES})
set_target_properties(gnuradio-hermes_ofdm PROPERTIES DEFINE_SYMBOL "gnuradio_hermes_ofdm_EXPORTS")

if(APPLE)
    set_target_properties(gnuradio-hermes_ofdm PROPERTIES
        INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/lib"
    )
endif(APPLE)

########################################################################
# Install built library files
########################################################################
include(GrMiscUtils)
GR_LIBRARY_FOO(gnuradio-hermes_ofdm RUNTIME_COMPONENT "hermes_ofdm_runtime" DEVEL_COMPONENT "hermes_ofdm_devel")

########################################################################
# Build and register unit test
########################################################################
include(GrTest)

include_directories(${CPPUNIT_INCLUDE_DIRS})

list(APPEND test_hermes_ofdm_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/test_hermes_ofdm.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/qa_hermes_ofdm.cc
)

add_executable(test-hermes_ofdm ${test_hermes_ofdm_sources})

target_link_libraries(
  test-hermes_ofdm
  ${GNURADIO_RUNTIME_LIBRARIES}
  ${Boost_LIBRARIES}
  ${CPPUNIT_LIBRARIES}
  gnuradio-hermes_ofdm
)

GR_ADD_TEST(test_hermes_ofdm test-hermes_ofdm)

########################################################################
# Print summary
########################################################################
message(STATUS "Using install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Building for version: ${VERSION} / ${LIBVER}")

