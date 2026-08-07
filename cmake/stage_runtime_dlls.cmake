if (NOT WIN32)
  return()
endif()

if (NOT DEFINED OUTPUT_DIR OR NOT DEFINED SOURCE_ROOT OR NOT DEFINED CONFIG)
  message(FATAL_ERROR "stage_runtime_dlls.cmake requires OUTPUT_DIR, SOURCE_ROOT, and CONFIG")
endif()

set(_runtime_dir "${SOURCE_ROOT}/vcpkg_installed/x64-windows/debug/bin")
if (NOT CONFIG STREQUAL "Debug")
  set(_runtime_dir "${SOURCE_ROOT}/vcpkg_installed/x64-windows/bin")
endif()

if (NOT EXISTS "${_runtime_dir}")
  return()
endif()

file(GLOB _runtime_dlls "${_runtime_dir}/*.dll")
foreach(_dll IN LISTS _runtime_dlls)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "${OUTPUT_DIR}"
    RESULT_VARIABLE _copy_result)
  if (NOT _copy_result EQUAL 0)
    message(FATAL_ERROR "Failed to stage runtime DLL '${_dll}' to '${OUTPUT_DIR}'")
  endif()
endforeach()
