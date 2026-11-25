# Native, per-user installers. No service, firewall or audio driver is registered automatically.
if(NOT WIN32)
  return()
endif()
function(openaoip_installer kind app)
  set(payloads "$<TARGET_FILE:${app}>|$<TARGET_FILE_NAME:${app}>" "${CMAKE_SOURCE_DIR}/WINDOWS-START.md|WINDOWS-START.md"
    "${CMAKE_SOURCE_DIR}/WINDOWS-AUDIO.md|WINDOWS-AUDIO.md"
    "${CMAKE_SOURCE_DIR}/ARCHITECTURE.md|ARCHITECTURE.md"
    "${CMAKE_SOURCE_DIR}/LICENSE|LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md|THIRD_PARTY.md"
    "${CMAKE_SOURCE_DIR}/drivers/wdm/README.md|drivers/wdm/README.md")
  list(APPEND payloads "${CMAKE_SOURCE_DIR}/drivers/wdm/VM-TEST.md|drivers/wdm/VM-TEST.md")
  foreach(name httplib json miniaudio)
    list(APPEND payloads "${CMAKE_SOURCE_DIR}/third_party/LICENSE-${name}.txt|licenses/LICENSE-${name}.txt")
  endforeach()
  set(dependencies ${app})
  if(kind STREQUAL "Suite")
    list(APPEND dependencies OpenAoIPVirtualSoundcard OpenAoIPAudioBridge)
    list(APPEND payloads "$<TARGET_FILE:OpenAoIPVirtualSoundcard>|$<TARGET_FILE_NAME:OpenAoIPVirtualSoundcard>"
      "$<TARGET_FILE:OpenAoIPAudioBridge>|$<TARGET_FILE_NAME:OpenAoIPAudioBridge>")
  endif()
  if(NOT kind STREQUAL "Controller")
    list(APPEND dependencies OpenAoIPEngine)
    list(APPEND payloads "$<TARGET_FILE:OpenAoIPEngine>|$<TARGET_FILE_NAME:OpenAoIPEngine>")
  endif()
  if(kind STREQUAL "VirtualSoundcard" OR kind STREQUAL "Suite")
    list(APPEND dependencies OpenAoIPASIO64 OpenAoIPWdmSetup)
    list(APPEND payloads "$<TARGET_FILE:OpenAoIPWdmSetup>|OpenAoIPWdmSetup.exe")
    list(APPEND payloads "$<TARGET_FILE:OpenAoIPASIO64>|OpenAoIPASIO64.dll"
      "${CMAKE_SOURCE_DIR}/third_party/asio-sdk/COPYING.GPL-3.0.txt|asio-source/COPYING"
      "${CMAKE_SOURCE_DIR}/third_party/asio-sdk/ASIOSDK/LICENSE.txt|asio-source/SDK-LICENSE.txt"
      "${CMAKE_SOURCE_DIR}/drivers/asio/LICENSE.md|asio-source/LICENSE.md"
      "${CMAKE_SOURCE_DIR}/drivers/asio/BUILD-SOURCE.md|asio-source/BUILD-SOURCE.md"
      "${CMAKE_SOURCE_DIR}/drivers/asio/standalone.cmake|asio-source/CMakeLists.txt"
      "${CMAKE_SOURCE_DIR}/LICENSE|asio-source/TRANSPORT-LICENSE.txt"
      "${CMAKE_SOURCE_DIR}/src/virtual_bridge.cpp|asio-source/src/virtual_bridge.cpp"
      "${CMAKE_SOURCE_DIR}/include/aoip/virtual_bridge.hpp|asio-source/include/aoip/virtual_bridge.hpp")
    foreach(name openaoip_asio.cpp openaoip_asio.def driver_id.hpp asio_api.hpp)
      list(APPEND payloads "${CMAKE_SOURCE_DIR}/drivers/asio/${name}|asio-source/driver/${name}")
    endforeach()
    file(GLOB sdk_headers "${CMAKE_SOURCE_DIR}/third_party/asio-sdk/ASIOSDK/common/*.h")
    foreach(path IN LISTS sdk_headers)
      get_filename_component(name "${path}" NAME)
      list(APPEND payloads "${path}|asio-source/sdk/${name}")
    endforeach()
  endif()
  set(contents "")
  foreach(entry IN LISTS payloads)
    string(APPEND contents "${entry}\n")
    string(REGEX REPLACE "\\|.*$" "" source "${entry}")
    list(APPEND dependencies "${source}")
    string(REGEX REPLACE "^.*\\|" "" relative "${entry}")
    get_filename_component(destination "${relative}" DIRECTORY)
    get_filename_component(name "${relative}" NAME)
    if(destination STREQUAL "")
      set(destination ".")
    endif()
    install(FILES "${source}" DESTINATION "${destination}" RENAME "${name}" COMPONENT ${kind})
  endforeach()
  set(generated "${CMAKE_BINARY_DIR}/installer-${kind}/$<CONFIG>")
  file(GENERATE OUTPUT "${generated}/payload.txt" CONTENT "${contents}")
  add_custom_command(OUTPUT "${generated}/payload.rc"
    COMMAND ${CMAKE_COMMAND} "-DMANIFEST=${generated}/payload.txt" "-DOUTPUT=${generated}/payload.rc"
      "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}" -P "${CMAKE_SOURCE_DIR}/cmake/installer_payload.cmake"
    DEPENDS ${dependencies} "${generated}/payload.txt" "${CMAKE_SOURCE_DIR}/cmake/installer_payload.cmake" VERBATIM)
  add_executable(OpenAoIP${kind}Setup WIN32 src/native/setup.cpp "${generated}/payload.rc")
  target_compile_definitions(OpenAoIP${kind}Setup PRIVATE UNICODE _UNICODE "OPENAOIP_PRODUCT=L\"${kind}\"")
  target_compile_options(OpenAoIP${kind}Setup PRIVATE /utf-8 /EHsc)
  target_link_options(OpenAoIP${kind}Setup PRIVATE /MANIFEST:NO)
  target_link_libraries(OpenAoIP${kind}Setup PRIVATE aoip_native_ui)
  install(TARGETS OpenAoIP${kind}Setup RUNTIME DESTINATION . COMPONENT ${kind})
endfunction()
openaoip_installer(Controller OpenAoIPController)
openaoip_installer(VirtualSoundcard OpenAoIPVirtualSoundcard)
openaoip_installer(AudioBridge OpenAoIPAudioBridge)
openaoip_installer(Suite OpenAoIPController)
