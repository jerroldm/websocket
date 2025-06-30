idf_component_register(
    SRCS "main.c" "manage_time.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer freertos log websocket_client sim7670g_modem
)
