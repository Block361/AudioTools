# Optional development build only. This never signs, registers or installs a driver.
set(OPENAOIP_WDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/wdk/c" CACHE PATH "Project-local or installed Windows Driver Kit root")
set(OPENAOIP_WDK_VERSION "10.0.26100.0" CACHE STRING "Matching WDK/SDK headers and library version")
if(WIN32 AND MSVC AND EXISTS "${OPENAOIP_WDK_ROOT}/Include/${OPENAOIP_WDK_VERSION}/km/portcls.h")
  set(_wdk_inc "${OPENAOIP_WDK_ROOT}/Include/${OPENAOIP_WDK_VERSION}")
  set(_wdk_lib "${OPENAOIP_WDK_ROOT}/Lib/${OPENAOIP_WDK_VERSION}/km/x64")
  set(_sdk_inc "C:/Program Files (x86)/Windows Kits/10/Include/${OPENAOIP_WDK_VERSION}")
  get_filename_component(_vc_root "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(_unused RANGE 1 3)
    get_filename_component(_vc_root "${_vc_root}" DIRECTORY)
  endforeach()
  set(_driver_dir "${CMAKE_BINARY_DIR}/wdm-development")
  add_custom_command(OUTPUT "${_driver_dir}/OpenAoIPWdm.sys"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_driver_dir}"
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /c /kernel /GR- /EHs-c- /Zl /GS /std:c++17 /W3
      /D_AMD64_ /DAMD64 /D_WIN64 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000010
      "/I${_wdk_inc}/km" "/I${_wdk_inc}/km/crt" "/I${_sdk_inc}/shared" "/I${_sdk_inc}/ucrt" "/I${_vc_root}/include"
      "/Fo${_driver_dir}/openaoip_wdm.obj" "${CMAKE_SOURCE_DIR}/drivers/wdm/openaoip_wdm.cpp"
    COMMAND "${CMAKE_LINKER}" /NOLOGO /DRIVER /SUBSYSTEM:NATIVE /ENTRY:GsDriverEntry /NODEFAULTLIB /MACHINE:X64 /MANIFEST:NO /INCREMENTAL:NO /OPT:REF /OPT:ICF
      "/OUT:${_driver_dir}/OpenAoIPWdm.sys" "${_driver_dir}/openaoip_wdm.obj"
      "${_wdk_lib}/ntoskrnl.lib" "${_wdk_lib}/hal.lib" "${_wdk_lib}/portcls.lib" "${_wdk_lib}/wdmsec.lib" "${_wdk_lib}/bufferoverflowfastfailk.lib"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/drivers/wdm/OpenAoIPWdm.inf" "${_driver_dir}/OpenAoIPWdm.inf"
    DEPENDS drivers/wdm/openaoip_wdm.cpp drivers/wdm/protocol.h drivers/wdm/audio_queue.hpp drivers/wdm/OpenAoIPWdm.inf
    VERBATIM)
  add_custom_target(OpenAoIPWdm DEPENDS "${_driver_dir}/OpenAoIPWdm.sys")
endif()
