idf_component_register(
    SRCS "main.c" "wifi.c" "http_stream.c" "display.c" "display_gfx.c" "touch_xpt2046.c" "app_launcher.c" "roboeyes_display.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_netif esp_http_server esp_timer esp_lcd esp_lcd_ili9341 driver RoboEyes esp_system
)