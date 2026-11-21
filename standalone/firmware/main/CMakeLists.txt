idf_component_register(
    SRCS "main.c" "board.c" "display.c" "touch.c" "audio.c" "protocol.c" "settings.c"
         "ui.c" "clock_weather.c" "net.c" "formutil.c" "assist_util.c" "assistant.c" "sysctl.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd_st77916 driver esp_lcd esp_timer
             esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client
             esp-tls json lwip mbedtls espressif__esp-sr)

idf_component_get_property(esp_sr_dir espressif__esp-sr COMPONENT_DIR)
set(SRMODELS_BIN "${CMAKE_CURRENT_BINARY_DIR}/srmodels.bin")
add_custom_command(
    OUTPUT "${SRMODELS_BIN}"
    COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/pack_wake_models.py" "${esp_sr_dir}" "${SRMODELS_BIN}"
    DEPENDS "${CMAKE_SOURCE_DIR}/pack_wake_models.py"
    COMMENT "Packing WakeNet9 English models into srmodels.bin"
    VERBATIM
)
add_custom_target(pack_srmodels ALL DEPENDS "${SRMODELS_BIN}")
add_dependencies(${COMPONENT_LIB} pack_srmodels)
target_add_binary_data(${COMPONENT_LIB} "${SRMODELS_BIN}" BINARY)
