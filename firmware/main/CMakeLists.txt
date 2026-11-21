idf_component_register(
    SRCS "main.c" "board.c" "display.c" "touch.c" "audio.c" "protocol.c" "settings.c"
         "ui.c" "clock_weather.c" "net.c" "formutil.c" "bridge_client.c"
         "usb_frame.c" "usb_link.c" "sysctl.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd_st77916 driver esp_driver_usb_serial_jtag esp_lcd esp_timer
             esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client
             esp-tls json lwip mbedtls)
