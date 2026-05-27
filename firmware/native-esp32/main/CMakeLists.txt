if(GPIO_DEBUG_MODE AND NOT ALLOW_DANGEROUS_DEBUG)
    message(FATAL_ERROR
        "GPIO_DEBUG_MODE=1 blocks normal boot and OTA. "
        "Pass -DALLOW_DANGEROUS_DEBUG=1 only for deliberate bench bring-up.")
endif()

if(RECOVERY_FIRMWARE)
    set(TR22_MAIN_SRCS
        "recovery_main.c"
        "recovery_boot.c"
        "sd_recovery.c"
        "tr19_epaper.c"
        "wifi_connect.c"
        "ota_update.c"
        "badge_i2c.c"
        "badge_power.c"
        "log_ship.c"
    )
else()
    set(TR22_MAIN_SRCS
        "main.c"
        "recovery_boot.c"
        "gpio_debug.c"
        "tr19_epaper.c"
        "wifi_connect.c"
        "ota_update.c"
        "badge_state.c"
        "badge_i2c.c"
        "badge_power.c"
        "badge_led.c"
        "badge_server.c"
        "badge_button.c"
        "badge_touch.c"
        "log_ship.c"
    )
endif()

idf_component_register(
    SRCS ${TR22_MAIN_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_http_server
        esp_pm
        esp_driver_rmt
        esp_driver_i2c
        app_update
        nvs_flash
        driver
        esp_driver_sdmmc
        fatfs
        sdmmc
        esp_adc
        esp_timer
        spi_flash
        bootloader_support
        json
)

if(NOT DEFINED BADGE_BUILD_TIMESTAMP)
    execute_process(
        COMMAND date "+%b %d %Y %H:%M:%S %Z"
        OUTPUT_VARIABLE BADGE_BUILD_TIMESTAMP
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

string(TOUPPER "${BADGE_BUILD_TIMESTAMP}" BADGE_BUILD_TIMESTAMP_UPPER)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/badge_build_info.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/badge_build_info.h"
    @ONLY
)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Forward the GPIO_DEBUG_MODE cmake variable (-DGPIO_DEBUG_MODE=1) to the C
# preprocessor so that main.c and gpio_debug.c can see it.
if(GPIO_DEBUG_MODE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC GPIO_DEBUG_MODE=1)
endif()

if(RECOVERY_FIRMWARE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC RECOVERY_FIRMWARE=1)
endif()

# esp_app_desc.c embeds __DATE__/__TIME__. Make it rebuild even when only
# application sources changed so OTA can distinguish successive local builds.
idf_build_get_property(idf_path IDF_PATH)
add_custom_target(refresh_app_desc
    COMMAND ${CMAKE_COMMAND} -E touch
            "${idf_path}/components/esp_app_format/esp_app_desc.c"
)
add_dependencies(${COMPONENT_LIB} refresh_app_desc)
if(TARGET __idf_esp_app_format)
    add_dependencies(__idf_esp_app_format refresh_app_desc)
endif()
