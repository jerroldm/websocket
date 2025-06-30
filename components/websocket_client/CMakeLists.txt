idf_component_register(
    SRCS "websocket_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer freertos log mbedtls modem_tcp
)
