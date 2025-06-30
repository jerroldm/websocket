idf_component_register(
    SRCS "sim7670g_modem.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer freertos log
)
