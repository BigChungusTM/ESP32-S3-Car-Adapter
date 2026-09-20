cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airplay_ipod_bridge)

# Raw upstream TinyUSB has no IDF additions layer, so nothing provides
# tusb_config.h on its own include path. Export our config dir to it.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ipod_usb/include")

# Reproducible compatibility fixes for the pinned TinyUSB component.
idf_component_get_property(tusb_dir espressif__tinyusb COMPONENT_DIR)
set(patched_dir "${CMAKE_BINARY_DIR}/tinyusb_patched")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_tinyusb.py"
    "${tusb_dir}" "${patched_dir}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(tusb_sources ${tusb_lib} SOURCES)
foreach(unit usbd audio_device dcd_dwc2)
    list(FILTER tusb_sources EXCLUDE REGEX "/${unit}\\.c$")
    list(APPEND tusb_sources "${patched_dir}/${unit}.c")
endforeach()
set_property(TARGET ${tusb_lib} PROPERTY SOURCES "${tusb_sources}")
target_include_directories(${tusb_lib} PRIVATE
    "${tusb_dir}/src/class/audio" "${tusb_dir}/src/portable/synopsys/dwc2")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_tinyusb.py")
