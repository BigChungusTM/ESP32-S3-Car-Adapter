cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(airplay_ipod_bridge)

# Raw upstream TinyUSB has no IDF additions layer, so nothing provides
# tusb_config.h on its own include path. Export our config dir to it.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/components/ipod_usb/include")
