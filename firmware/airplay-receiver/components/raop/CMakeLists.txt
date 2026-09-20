idf_component_register(SRCS raop.c rtp.c util.c dmap_parser.c INCLUDE_DIRS . REQUIRES esp_netif esp_timer esp_hw_support esp_system mbedtls mdns alac pthread)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=implicit-function-declaration -Wno-error=misleading-indentation -Wno-error=format -Wno-error=incompatible-pointer-types)
