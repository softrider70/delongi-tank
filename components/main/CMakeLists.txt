idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "../../include"
    REQUIRES 
        nvs_flash 
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_touch_sens
        esp_wifi
        esp_http_server
        mbedtls
        esp_timer
    WHOLE_ARCHIVE
)

add_custom_target(generate_version_header ALL
    COMMAND python ${CMAKE_SOURCE_DIR}/tools/increment_build.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/include/version.h
    COMMENT "Generating version.h and incrementing build number"
    VERBATIM
)

add_dependencies(${COMPONENT_LIB} generate_version_header)
