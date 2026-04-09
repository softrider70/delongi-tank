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
        esp_https_ota
        app_update
        mbedtls
        esp_timer
    WHOLE_ARCHIVE
)

set(VERSION_HEADER ${CMAKE_SOURCE_DIR}/include/version.h)
set(VERSION_TEMPLATE ${CMAKE_SOURCE_DIR}/include/version.h.in)

add_custom_command(
    OUTPUT ${VERSION_HEADER}
    COMMAND python ${CMAKE_SOURCE_DIR}/tools/increment_build.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS ${VERSION_TEMPLATE} ${CMAKE_SOURCE_DIR}/include/config.h ${CMAKE_SOURCE_DIR}/tools/increment_build.py
    COMMENT "Generating version.h and incrementing build number"
    VERBATIM
)

add_custom_target(generate_version_header DEPENDS ${VERSION_HEADER})
add_dependencies(${COMPONENT_LIB} generate_version_header)
