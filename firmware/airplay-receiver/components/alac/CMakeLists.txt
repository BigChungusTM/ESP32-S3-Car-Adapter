idf_component_register(INCLUDE_DIRS .)
add_prebuilt_library(alac_decoder libalac.a)
target_link_libraries(${COMPONENT_LIB} INTERFACE alac_decoder)
