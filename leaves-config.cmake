
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was leaves-config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include(CMakeFindDependencyMacro)

option(LEAVES_LOG "Enable leaves logging macros for consumers" OFF)
option(LEAVES_SINGLE_PROCESS "Enable leaves single-process macros for consumers" OFF)

set(_leaves_boost_components headers)
set(leaves_replication_FOUND FALSE)

find_dependency(Boost 1.80 REQUIRED COMPONENTS ${_leaves_boost_components})

include("${CMAKE_CURRENT_LIST_DIR}/leaves-targets.cmake")

if(LEAVES_LOG AND TARGET leaves::leaves)
	target_compile_definitions(leaves::leaves INTERFACE LEAVES_LOG)
endif()

if(LEAVES_SINGLE_PROCESS AND TARGET leaves::leaves)
	target_compile_definitions(leaves::leaves INTERFACE LEAVES_SINGLE_PROCESS)
endif()

set(_leaves_needs_replication FALSE)
if("replication" IN_LIST leaves_FIND_COMPONENTS)
	set(_leaves_needs_replication TRUE)
endif()

if(_leaves_needs_replication)
	if(NOT TARGET BLAKE3::blake3)
		find_dependency(blake3 CONFIG REQUIRED)
	endif()
elseif(NOT TARGET BLAKE3::blake3)
	find_package(blake3 CONFIG QUIET)
endif()

if(TARGET BLAKE3::blake3)
	include("${CMAKE_CURRENT_LIST_DIR}/leaves-replication-targets.cmake")
	set(leaves_replication_FOUND TRUE)
endif()

check_required_components(leaves)
