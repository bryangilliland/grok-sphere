# Vendored copy of Espressif's esp_lcd_st77916 driver as shipped in Waveshare's
# ESP32-S3-Touch-LCD-1.85C demo (Apache-2.0). Kept local so the build does not
# depend on a newer registry version with a different API.
idf_component_register(SRCS "esp_lcd_st77916.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd driver)
