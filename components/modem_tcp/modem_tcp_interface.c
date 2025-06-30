#include "modem_tcp_interface.h"
#include "sim7670g_modem.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MODEM_TCP";

esp_err_t modem_tcp_connect(const char *host, int port)
{
    if (!host || port <= 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting TCP to %s:%d via SIM7670G", host, port);

    // Use the SIM7670G component TCP connect function
    esp_err_t ret = sim7670g_tcp_connect(host, port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SIM7670G TCP connection failed");
        return ret;
    }

    ESP_LOGI(TAG, "TCP connected successfully to %s:%d", host, port);
    return ESP_OK;
}

esp_err_t modem_tcp_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting TCP connection");
    
    esp_err_t ret = sim7670g_tcp_disconnect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TCP disconnect warning, but continuing");
    }

    ESP_LOGI(TAG, "TCP disconnected");
    return ESP_OK;
}

esp_err_t modem_tcp_send(const char *data, size_t length)
{
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Sending %zu bytes via TCP", length);

    esp_err_t ret = sim7670g_tcp_send(data, length);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCP send failed");
        return ret;
    }

    ESP_LOGD(TAG, "TCP send successful");
    return ESP_OK;
}

esp_err_t modem_tcp_receive(char *buffer, size_t buffer_size, size_t *received_length, int timeout_ms)
{
    if (!buffer || buffer_size == 0 || !received_length) {
        return ESP_ERR_INVALID_ARG;
    }

    *received_length = 0;

    esp_err_t ret = sim7670g_tcp_receive(buffer, buffer_size, received_length, timeout_ms);
    
    if (ret == ESP_OK && *received_length > 0) {
        ESP_LOGD(TAG, "TCP received %zu bytes", *received_length);
    } else if (ret == ESP_ERR_TIMEOUT) {
        // Timeout is normal for non-blocking receive
        return ESP_ERR_TIMEOUT;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        // No data available
        return ESP_ERR_NOT_FOUND;
    }

    return ret;
}

bool modem_tcp_is_connected(void)
{
    return sim7670g_tcp_is_connected();
}

esp_err_t modem_tcp_get_status(char *status_buffer, size_t buffer_size)
{
    if (!status_buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get SIM7670G status
    sim7670g_status_t modem_status;
    esp_err_t ret = sim7670g_get_status(&modem_status);
    if (ret != ESP_OK) {
        return ret;
    }

    // Format status string
    snprintf(status_buffer, buffer_size,
             "SIM7670G Status:\n"
             "- Initialized: %s\n"
             "- AT Responsive: %s\n"
             "- SIM Status: %d\n"
             "- Registration: %d\n"
             "- Signal Quality: %d\n"
             "- PDP Active: %s\n"
             "- Local IP: %s\n"
             "- TCP Connected: %s",
             modem_status.initialized ? "Yes" : "No",
             modem_status.at_responsive ? "Yes" : "No",
             modem_status.sim_status,
             modem_status.registration_status,
             modem_status.signal_quality,
             modem_status.pdp_active ? "Yes" : "No",
             modem_status.local_ip,
             sim7670g_tcp_is_connected() ? "Yes" : "No");

    return ESP_OK;
}

esp_err_t modem_tcp_get_connection_info(char *host_buffer, size_t host_buffer_size, int *port)
{
    // This would need to be implemented in the SIM7670G component
    // For now, return error as this info isn't stored in the current implementation
    ESP_LOGW(TAG, "Connection info not available from SIM7670G component");
    return ESP_ERR_NOT_SUPPORTED;
}
