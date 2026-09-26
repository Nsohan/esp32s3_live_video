idf_component_register(
    SRCS "main.c" "wifi.c" "http_stream.c" "display.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_netif esp_http_server esp_timer esp_lcd esp_lcd_ili9341 driver
)