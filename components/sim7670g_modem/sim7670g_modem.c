#include "sim7670g_modem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "SIM7670G";

// Global modem state
static struct {
    sim7670g_config_t config;
    sim7670g_status_t status;
    sim7670g_event_callback_t event_callback;
    SemaphoreHandle_t uart_mutex;
    bool initialized;
    bool tcp_connected;
    char tcp_host[64];
    int tcp_port;
    char global_http_response[4096];  // Buffer for HTTP responses
} modem_state = {0};

// Buffer for AT command responses
#define AT_RESPONSE_BUFFER_SIZE 4096
static char at_response_buffer[AT_RESPONSE_BUFFER_SIZE];

// Forward declarations
static void notify_event(sim7670g_event_t event, int error_code, const char *message);
//static bool wait_for_response(const char *expected, int timeout_ms);
static void modem_uart_flush_input(void);
static bool send_at_command(const char *command, char *response, int timeout_ms);

// Implementation based on your existing code

esp_err_t sim7670g_init(sim7670g_config_t *config, sim7670g_event_callback_t event_callback)
{
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    if (modem_state.initialized) {
        ESP_LOGW(TAG, "Modem already initialized");
        return ESP_OK;
    }

    // Copy configuration
    memcpy(&modem_state.config, config, sizeof(sim7670g_config_t));
    modem_state.event_callback = event_callback;

    // Create mutex for UART access
    modem_state.uart_mutex = xSemaphoreCreateMutex();
    if (!modem_state.uart_mutex) {
        ESP_LOGE(TAG, "Failed to create UART mutex");
        return ESP_ERR_NO_MEM;
    }

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = (config->rts_pin >= 0 && config->cts_pin >= 0) ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };

    esp_err_t ret = uart_param_config(config->uart_port, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(modem_state.uart_mutex);
        return ret;
    }

    ret = uart_set_pin(config->uart_port, config->tx_pin, config->rx_pin, config->rts_pin, config->cts_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART pin config failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(modem_state.uart_mutex);
        return ret;
    }

    ret = uart_driver_install(config->uart_port, 1024, 1024, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(modem_state.uart_mutex);
        return ret;
    }

    // Configure power pin if provided
    if (config->power_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << config->power_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(config->power_pin, 1); // Assuming active high
    }

    // Configure reset pin if provided
    if (config->reset_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << config->reset_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(config->reset_pin, 1); // Assuming active low reset
    }

    // Configure pwrkey pin if provided
    if (config->pwrkey_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << config->pwrkey_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(config->pwrkey_pin, 1); // Power Key Sequence
    }

    // Initialize status
    memset(&modem_state.status, 0, sizeof(sim7670g_status_t));
    modem_state.status.initialized = true;
    modem_state.initialized = true;

    // Initialize global response buffer
    memset(modem_state.global_http_response, 0, sizeof(modem_state.global_http_response));

    ESP_LOGI(TAG, "SIM7670G modem initialized on UART%d", config->uart_port);
    notify_event(SIM7670G_EVENT_INITIALIZED, 0, "Modem initialized");

    return ESP_OK;
}

esp_err_t sim7670g_deinit(void)
{
    if (!modem_state.initialized) {
        return ESP_OK;
    }

    // Disconnect any active connections
    if (modem_state.tcp_connected) {
        sim7670g_tcp_disconnect();
    }

    // Deactivate PDP context
    if (modem_state.status.pdp_active) {
        sim7670g_deactivate_pdp_context();
    }

    // Uninstall UART driver
    uart_driver_delete(modem_state.config.uart_port);

    // Delete mutex
    if (modem_state.uart_mutex) {
        vSemaphoreDelete(modem_state.uart_mutex);
    }

    // Clear state
    memset(&modem_state, 0, sizeof(modem_state));

    ESP_LOGI(TAG, "SIM7670G modem deinitialized");
    return ESP_OK;
}

bool sim7670g_test_at(void)
{
    if (!modem_state.initialized) {
        return false;
    }

    bool result = send_at_command("AT", NULL, 1000);
    modem_state.status.at_responsive = result;
    return result;
}

// Internal AT command function (based on your original send_at_command)
static bool send_at_command(const char *command, char *response, int timeout_ms)
{
    if (!modem_state.initialized || !command) {
        return false;
    }

    // Take mutex for thread safety
    if (xSemaphoreTake(modem_state.uart_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take UART mutex");
        return false;
    }

    bool result = false;

    // Clear input buffer
    modem_uart_flush_input();

    // Send command
    char cmd_with_crlf[256];
    snprintf(cmd_with_crlf, sizeof(cmd_with_crlf), "%s\r\n", command);

    int len = uart_write_bytes(modem_state.config.uart_port, cmd_with_crlf, strlen(cmd_with_crlf));
    if (len < 0) {
        ESP_LOGE(TAG, "UART write failed");
        goto cleanup;
    }

    // Read response
    memset(at_response_buffer, 0, sizeof(at_response_buffer));

    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    size_t total_read = 0;

    while ((xTaskGetTickCount() - start_time) < timeout_ticks && total_read < (sizeof(at_response_buffer) - 1)) {
        int bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                       (uint8_t*)(at_response_buffer + total_read),
                                       sizeof(at_response_buffer) - total_read - 1,
                                       pdMS_TO_TICKS(100));

        if (bytes_read > 0) {
            total_read += bytes_read;
            at_response_buffer[total_read] = '\0';

            // Check for command completion
            if (strstr(at_response_buffer, "OK") ||
                strstr(at_response_buffer, "ERROR") ||
                strstr(at_response_buffer, "FAIL")) {
                break;
            }
        }
    }

    // Copy response if requested
    if (response && total_read > 0) {
        strcpy(response, at_response_buffer);
    }

    // Check for success
    result = (total_read > 0) && (strstr(at_response_buffer, "OK") != NULL);

    ESP_LOGI(TAG, "AT Command: %s", command);
    ESP_LOGI(TAG, "AT Response: %s", at_response_buffer);

cleanup:
    xSemaphoreGive(modem_state.uart_mutex);
    return result;
}

// Public AT command function
bool sim7670g_send_at_command(const char *command, char *response, size_t response_size, int timeout_ms)
{
    bool result = send_at_command(command, response, timeout_ms);

    // If response buffer is provided and smaller than our internal buffer, copy safely
    if (response && response_size > 0 && strlen(at_response_buffer) > 0) {
        strncpy(response, at_response_buffer, response_size - 1);
        response[response_size - 1] = '\0';
    }

    return result;
}

sim_status_t sim7670g_get_sim_status(void)
{
    char response[256];

    if (!send_at_command("AT+CPIN?", response, 3000)) {
        modem_state.status.sim_status = SIM_ERROR;
        return SIM_ERROR;
    }

    if (strstr(response, "READY")) {
        modem_state.status.sim_status = SIM_READY;
        if (modem_state.event_callback) {
            notify_event(SIM7670G_EVENT_SIM_READY, 0, "SIM card ready");
        }
        return SIM_READY;
    } else if (strstr(response, "SIM PIN")) {
        modem_state.status.sim_status = SIM_LOCKED;
        return SIM_LOCKED;
    } else {
        modem_state.status.sim_status = SIM_ERROR;
        return SIM_ERROR;
    }
}

bool sim7670g_sim_unlock(const char *pin)
{
    if (!pin) {
        return false;
    }

    char command[64];
    snprintf(command, sizeof(command), "AT+CPIN=\"%s\"", pin);

    return send_at_command(command, NULL, 3000);
}

bool sim7670g_set_apn(const char *apn)
{
    if (!apn) {
        return false;
    }

    char command[128];
    snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);

    return send_at_command(command, NULL, 3000);
}

reg_status_t sim7670g_get_registration_status(void)
{
    char response[256];

    if (!send_at_command("AT+CREG?", response, 3000)) {
        modem_state.status.registration_status = REG_UNKNOWN;
        return REG_UNKNOWN;
    }

    // Parse response: +CREG: n,stat
    char *stat_start = strstr(response, "+CREG:");
    if (!stat_start) {
        modem_state.status.registration_status = REG_UNKNOWN;
        return REG_UNKNOWN;
    }

    // Find the status value (after the comma)
    char *comma = strchr(stat_start, ',');
    if (!comma) {
        modem_state.status.registration_status = REG_UNKNOWN;
        return REG_UNKNOWN;
    }

    int status = atoi(comma + 1);

    reg_status_t reg_status;
    switch (status) {
        case 0: reg_status = REG_NOT_REGISTERED; break;
        case 1: reg_status = REG_OK_HOME; break;
        case 2: reg_status = REG_SEARCHING; break;
        case 3: reg_status = REG_DENIED; break;
        case 5: reg_status = REG_OK_ROAMING; break;
        default: reg_status = REG_UNKNOWN; break;
    }

    modem_state.status.registration_status = reg_status;

    if (reg_status == REG_OK_HOME || reg_status == REG_OK_ROAMING) {
        if (modem_state.event_callback) {
            notify_event(SIM7670G_EVENT_NETWORK_REGISTERED, 0, "Network registered");
        }
    }

    return reg_status;
}

int16_t sim7670g_get_signal_quality(void)
{
    char response[256];

    if (!send_at_command("AT+CSQ", response, 3000)) {
        modem_state.status.signal_quality = 99; // Unknown
        return 99;
    }

    // Parse response: +CSQ: rssi,ber
    char *csq_start = strstr(response, "+CSQ:");
    if (!csq_start) {
        modem_state.status.signal_quality = 99;
        return 99;
    }

    int rssi = atoi(csq_start + 6); // Skip "+CSQ: "

    modem_state.status.signal_quality = rssi;
    return rssi;
}

bool sim7670g_activate_pdp_context(void)
{
    bool result = send_at_command("AT+CGACT=1,1", NULL, 10000);
    modem_state.status.pdp_active = result;

    if (result && modem_state.event_callback) {
        notify_event(SIM7670G_EVENT_PDP_ACTIVATED, 0, "PDP context activated");
    }

    return result;
}

bool sim7670g_deactivate_pdp_context(void)
{
    bool result = send_at_command("AT+CGACT=0,1", NULL, 3000);
    modem_state.status.pdp_active = !result;
    return result;
}

bool sim7670g_get_local_ip(char *ip_buffer, size_t buffer_size)
{
    if (!ip_buffer || buffer_size == 0) {
        return false;
    }

    char response[256];
    if (!send_at_command("AT+CGPADDR=1", response, 3000)) {
        return false;
    }

    // Parse response: +CGPADDR: 1,ip_address
    char *ip_start = strstr(response, "+CGPADDR:");
    if (!ip_start) {
        return false;
    }

    // Find the IP address after the comma
    char *comma = strchr(ip_start, ',');
    if (!comma) {
        return false;
    }

    // Skip quotes if present
    char *ip_addr = comma + 1;
    if (*ip_addr == '"') {
        ip_addr++;
    }

    // Copy IP address
    size_t i = 0;
    while (i < (buffer_size - 1) && ip_addr[i] &&
           ip_addr[i] != '"' && ip_addr[i] != '\r' && ip_addr[i] != '\n') {
        ip_buffer[i] = ip_addr[i];
        i++;
    }
    ip_buffer[i] = '\0';

    // Store in status
    strncpy(modem_state.status.local_ip, ip_buffer, sizeof(modem_state.status.local_ip) - 1);
    modem_state.status.local_ip[sizeof(modem_state.status.local_ip) - 1] = '\0';

    return strlen(ip_buffer) > 0;
}

bool sim7670g_get_operator(char *operator_buffer, size_t buffer_size)
{
    if (!operator_buffer || buffer_size == 0) {
        return false;
    }

    char response[256];
    if (!send_at_command("AT+COPS?", response, 3000)) {
        return false;
    }

    // Parse response and extract operator name
    char *cops_start = strstr(response, "+COPS:");
    if (!cops_start) {
        return false;
    }

    // Find operator name (usually in quotes)
    char *quote_start = strchr(cops_start, '"');
    if (!quote_start) {
        return false;
    }

    char *quote_end = strchr(quote_start + 1, '"');
    if (!quote_end) {
        return false;
    }

    size_t name_len = quote_end - quote_start - 1;
    if (name_len >= buffer_size) {
        name_len = buffer_size - 1;
    }

    strncpy(operator_buffer, quote_start + 1, name_len);
    operator_buffer[name_len] = '\0';

    // Store in status
    strncpy(modem_state.status.operator_name, operator_buffer, sizeof(modem_state.status.operator_name) - 1);
    modem_state.status.operator_name[sizeof(modem_state.status.operator_name) - 1] = '\0';

    return true;
}

esp_err_t sim7670g_get_status(sim7670g_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    // Update current status (don't call functions that might take too long)
    memcpy(status, &modem_state.status, sizeof(sim7670g_status_t));
    return ESP_OK;
}

bool sim7670g_is_ready(void)
{
    return modem_state.initialized &&
           modem_state.status.at_responsive &&
           modem_state.status.sim_status == SIM_READY &&
           (modem_state.status.registration_status == REG_OK_HOME ||
            modem_state.status.registration_status == REG_OK_ROAMING) &&
           modem_state.status.pdp_active;
}

// TCP Functions
esp_err_t sim7670g_tcp_connect(const char *host, int port)
{
    if (!host || port <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!sim7670g_is_ready()) {
        ESP_LOGE(TAG, "Modem not ready for TCP connection");
        return ESP_ERR_INVALID_STATE;
    }

    char command[128];
    char response[256];

    // Open network (if not already open)
    send_at_command("AT+NETOPEN", response, 3000);

    // Check if network is open
    if (strstr(response, "+NETOPEN: 0") || strstr(response, "already opened")) {
        ESP_LOGI(TAG, "Network is ready for TCP connections");
    } else {
        ESP_LOGE(TAG, "Network not available: %s", response);
        return ESP_FAIL;
    }

    // Close socket 0 first
    send_at_command("AT+CIPCLOSE=0", response, 3000);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Connect on socket 0
    snprintf(command, sizeof(command), "AT+CIPOPEN=0,\"TCP\",\"%s\",%d", host, port);

    if (!send_at_command(command, response, 15000)) {
        ESP_LOGE(TAG, "TCP connect command failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "CIPOPEN response: %s", response);

    // For this modem, "OK" response means the connection attempt was accepted
    // We need to wait and check the actual connection status
    if (strstr(response, "OK")) {
        ESP_LOGI(TAG, "TCP connection command accepted, waiting for connection...");

        // Wait a moment for connection to establish
        vTaskDelay(pdMS_TO_TICKS(2000));

        // Check connection status
        if (send_at_command("AT+CIPOPEN?", response, 3000)) {
            ESP_LOGI(TAG, "Connection status: %s", response);

            // Look for our connection in the status
            char expected[64];
            snprintf(expected, sizeof(expected), "0,\"TCP\",\"%s\",%d", host, port);

            if (strstr(response, expected)) {
                ESP_LOGI(TAG, "TCP connection established successfully");

                // Store connection info
                strncpy(modem_state.tcp_host, host, sizeof(modem_state.tcp_host) - 1);
                modem_state.tcp_host[sizeof(modem_state.tcp_host) - 1] = '\0';
                modem_state.tcp_port = port;
                modem_state.tcp_connected = true;

                ESP_LOGI(TAG, "TCP connected to %s:%d", host, port);
                return ESP_OK;
            }
        }
    }

    ESP_LOGE(TAG, "TCP connection failed");
    return ESP_FAIL;
}

esp_err_t sim7670g_tcp_disconnect(void)
{
    if (!modem_state.tcp_connected) {
        return ESP_OK;
    }

    // Close TCP connection
    send_at_command("AT+CIPCLOSE=0", NULL, 3000);

    modem_state.tcp_connected = false;
    memset(modem_state.tcp_host, 0, sizeof(modem_state.tcp_host));
    modem_state.tcp_port = 0;

    ESP_LOGI(TAG, "TCP disconnected");
    return ESP_OK;
}

esp_err_t sim7670g_tcp_send(const char *data, size_t length)
{
    if (!modem_state.tcp_connected || !data || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char command[128];
    char cmd_with_crlf[128];
    char prompt_buffer[256];
    char status_buffer[128];

    // Check if data contains binary (non-printable) characters
    bool has_binary = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];
        // Allow printable ASCII (32-126) plus common whitespace (9=tab, 10=LF, 13=CR, 32=space)
        if (c < 9 || (c > 13 && c < 32) || c > 126) {
            has_binary = true;
            break;
        }
    }

    // Take mutex for the entire send operation
    if (xSemaphoreTake(modem_state.uart_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take UART mutex for TCP send");
        return ESP_FAIL;
    }

    bool success = false;
    int retry_count = 0;
    const int max_retries = 2;

    while (retry_count < max_retries && !success) {
        // Check TCP connection state
        uart_flush(modem_state.config.uart_port);
        uart_write_bytes(modem_state.config.uart_port, "AT+CIPOPEN?\r\n", strlen("AT+CIPOPEN?\r\n"));
        int bytes_read = uart_read_bytes(modem_state.config.uart_port, (uint8_t*)status_buffer, sizeof(status_buffer) - 1, pdMS_TO_TICKS(1000));
        if (bytes_read > 0) {
            status_buffer[bytes_read] = '\0';
            ESP_LOGI(TAG, "CIPOPEN status: %s", status_buffer);
            if (!strstr(status_buffer, "+CIPOPEN: 0,\"TCP\"")) {
                ESP_LOGE(TAG, "TCP connection not active");
                break;
            }
        }

        // Always use CIPSEND (skip CIPSENDEX for WebSocket frames)
        snprintf(command, sizeof(command), "AT+CIPSEND=0,%zu", length);
        int ret = snprintf(cmd_with_crlf, sizeof(cmd_with_crlf), "%s\r\n", command);
        if (ret >= sizeof(cmd_with_crlf)) {
            ESP_LOGE(TAG, "Command buffer too small for CRLF");
            break;
        }

        ESP_LOGI(TAG, "Sending command: %s", cmd_with_crlf);
        uart_flush(modem_state.config.uart_port);
        int len = uart_write_bytes(modem_state.config.uart_port, cmd_with_crlf, strlen(cmd_with_crlf));
        if (len < 0) {
            ESP_LOGE(TAG, "Failed to send CIPSEND command");
            retry_count++;
            continue;
        }

        // Wait for prompt ">"
        bool got_prompt = false;
        TickType_t start_time = xTaskGetTickCount();
        memset(prompt_buffer, 0, sizeof(prompt_buffer));
        size_t prompt_total = 0;

        while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(3000)) {
            bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                         (uint8_t*)(prompt_buffer + prompt_total),
                                         sizeof(prompt_buffer) - prompt_total - 1,
                                         pdMS_TO_TICKS(100));
            if (bytes_read > 0) {
                prompt_total += bytes_read;
                prompt_buffer[prompt_total] = '\0';
                ESP_LOGI(TAG, "UART received: %s", prompt_buffer);

                if (strstr(prompt_buffer, "ERROR")) {
                    ESP_LOGE(TAG, "Modem returned ERROR for CIPSEND");
                    break;
                }
                if (strstr(prompt_buffer, ">")) {
                    got_prompt = true;
                    break;
                }
            }
        }

        if (!got_prompt) {
            ESP_LOGE(TAG, "Did not receive send prompt '>'");
            retry_count++;
            continue;
        }

        // Send the data (raw binary for WebSocket frames)
        ESP_LOGI(TAG, "Sending data (%zu bytes) - %s", length, has_binary ? "binary" : "text");

        // Log first few bytes for debugging
        if (length >= 16) {
            ESP_LOGI(TAG, "First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     (uint8_t)data[0], (uint8_t)data[1], (uint8_t)data[2], (uint8_t)data[3],
                     (uint8_t)data[4], (uint8_t)data[5], (uint8_t)data[6], (uint8_t)data[7],
                     (uint8_t)data[8], (uint8_t)data[9], (uint8_t)data[10], (uint8_t)data[11],
                     (uint8_t)data[12], (uint8_t)data[13], (uint8_t)data[14], (uint8_t)data[15]);
        }

        int bytes_written = uart_write_bytes(modem_state.config.uart_port, data, length);
        if (bytes_written != length) {
            ESP_LOGE(TAG, "Failed to send all data bytes: %d/%zu", bytes_written, length);
            retry_count++;
            continue;
        }

        // Wait for send confirmation - improved for binary data
        memset(at_response_buffer, 0, sizeof(at_response_buffer));
        size_t total_read = 0;
        start_time = xTaskGetTickCount();
        bool send_ok_found = false;

        while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(10000) && total_read < (sizeof(at_response_buffer) - 1)) {
            bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                         (uint8_t*)(at_response_buffer + total_read),
                                         sizeof(at_response_buffer) - total_read - 1,
                                         pdMS_TO_TICKS(100));
            if (bytes_read > 0) {
                total_read += bytes_read;

                // Look for "SEND OK" pattern in binary response
                for (size_t i = 0; i <= total_read - 7; i++) {
                    if (memcmp(&at_response_buffer[i], "SEND OK", 7) == 0) {
                        send_ok_found = true;
                        success = true;
                        ESP_LOGI(TAG, "Found SEND OK confirmation");
                        goto response_done;
                    }
                }

                // Look for +CIPSEND: pattern
                for (size_t i = 0; i <= total_read - 9; i++) {
                    if (memcmp(&at_response_buffer[i], "+CIPSEND:", 9) == 0) {
                        send_ok_found = true;
                        success = true;
                        ESP_LOGI(TAG, "Found +CIPSEND confirmation");
                        goto response_done;
                    }
                }

                // Look for ERROR pattern
                for (size_t i = 0; i <= total_read - 5; i++) {
                    if (memcmp(&at_response_buffer[i], "ERROR", 5) == 0) {
                        ESP_LOGE(TAG, "Send failed with ERROR response");
                        goto response_done;
                    }
                }

                // Log response as hex for debugging (don't try to print as string)
                if (total_read >= 8) {
                    ESP_LOGD(TAG, "Response bytes: %02x %02x %02x %02x %02x %02x %02x %02x...",
                             (uint8_t)at_response_buffer[0], (uint8_t)at_response_buffer[1],
                             (uint8_t)at_response_buffer[2], (uint8_t)at_response_buffer[3],
                             (uint8_t)at_response_buffer[4], (uint8_t)at_response_buffer[5],
                             (uint8_t)at_response_buffer[6], (uint8_t)at_response_buffer[7]);
                }
            }
        }

response_done:
        if (!success && !send_ok_found) {
            ESP_LOGE(TAG, "TCP send confirmation not received");
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retry
        } else {
            ESP_LOGI(TAG, "TCP send successful (%zu bytes)", length);
        }
    }

    xSemaphoreGive(modem_state.uart_mutex);
    return success ? ESP_OK : ESP_FAIL;
}

esp_err_t sim7670g_tcp_receive(char *buffer, size_t buffer_size, size_t *received_length, int timeout_ms)
{
    if (!modem_state.tcp_connected || !buffer || buffer_size == 0 || !received_length) {
        return ESP_ERR_INVALID_ARG;
    }

    *received_length = 0;

    if (xSemaphoreTake(modem_state.uart_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGD(TAG, "UART mutex busy during receive (normal during send)");
        return ESP_ERR_TIMEOUT;
    }

    memset(at_response_buffer, 0, sizeof(at_response_buffer));
    size_t total_read = 0;
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_time) < timeout_ticks && total_read < (sizeof(at_response_buffer) - 1)) {
        int bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                       (uint8_t*)(at_response_buffer + total_read),
                                       sizeof(at_response_buffer) - total_read - 1,
                                       pdMS_TO_TICKS(100));

        if (bytes_read > 0) {
            total_read += bytes_read;
            at_response_buffer[total_read] = '\0';

            ESP_LOGI(TAG, "Raw TCP receive data (%zu bytes): %s", total_read, at_response_buffer);

            // Look for SIM7670G data patterns
            char *recv_start = strstr(at_response_buffer, "RECV FROM:");
            if (!recv_start) {
                recv_start = strstr(at_response_buffer, "+IPD");
            }

            if (recv_start) {
                ESP_LOGI(TAG, "Found data pattern starting at: %.50s", recv_start);

                // For "RECV FROM:" format, look for the actual HTTP data
                char *http_start = strstr(recv_start, "HTTP/");
                if (!http_start) {
                    // For "+IPD" format, look for HTTP after the length
                    char *ipd_pos = strstr(recv_start, "+IPD");
                    if (ipd_pos) {
                        // Skip "+IPD" and any length numbers to find HTTP data
                        char *search_pos = ipd_pos + 4;
                        while (*search_pos && (*search_pos < 'A' || *search_pos > 'Z')) {
                            search_pos++;
                        }
                        if (*search_pos) {
                            http_start = search_pos;
                        }
                    }
                }

                if (http_start) {
                    // Calculate available data length
                    size_t available_data = total_read - (http_start - at_response_buffer);
                    size_t copy_length = (available_data < buffer_size - 1) ? available_data : buffer_size - 1;

                    memcpy(buffer, http_start, copy_length);
                    buffer[copy_length] = '\0';
                    *received_length = copy_length;

                    xSemaphoreGive(modem_state.uart_mutex);
                    ESP_LOGI(TAG, "TCP received %zu bytes of HTTP data", copy_length);
                    return ESP_OK;
                }
            }
        }
    }

    xSemaphoreGive(modem_state.uart_mutex);

    if (total_read == 0) {
        ESP_LOGD(TAG, "TCP receive timeout - no data received");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "TCP receive completed but no HTTP data found in: %s", at_response_buffer);
    return ESP_ERR_NOT_FOUND;
}

bool sim7670g_tcp_is_connected(void)
{
    return modem_state.tcp_connected;
}

// HTTPS Functions (converted from your existing code)
bool sim7670g_https_begin(void)
{
    if (!sim7670g_is_ready()) {
        ESP_LOGE(TAG, "Modem not ready for HTTPS");
        return false;
    }

    // Initialize HTTPS
    if (!send_at_command("AT+HTTPSINIT", NULL, 3000)) {
        ESP_LOGE(TAG, "HTTPS init failed");
        return false;
    }

    // Set HTTPS parameters based on your original code
    send_at_command("AT+HTTPSOPSE=0,1", NULL, 3000);

    return true;
}

bool sim7670g_https_set_url(const char *url)
{
    if (!url) {
        return false;
    }

    char command[256];
    snprintf(command, sizeof(command), "AT+HTTPSOPSE=1,\"%s\"", url);

    return send_at_command(command, NULL, 3000);
}

int sim7670g_https_get(void)
{
    // Based on your original modem_https_get function
    char response[512];

    if (!send_at_command("AT+HTTPSGET", response, 30000)) {
        ESP_LOGE(TAG, "HTTPS GET failed");
        return -1;
    }

    // Parse response code from +HTTPSGET: response_code
    char *get_start = strstr(response, "+HTTPSGET:");
    if (!get_start) {
        ESP_LOGE(TAG, "Invalid HTTPS GET response format");
        return -1;
    }

    int response_code = atoi(get_start + 10); // Skip "+HTTPSGET:"
    ESP_LOGI(TAG, "HTTPS GET response code: %d", response_code);

    return response_code;
}

bool sim7670g_https_get_header(char *header_buffer, size_t buffer_size)
{
    if (!header_buffer || buffer_size == 0) {
        return false;
    }

    // Based on your original modem_https_get_header function
    char response[1024];
    if (!send_at_command("AT+HTTPSHEAD", response, 10000)) {
        ESP_LOGE(TAG, "Failed to get HTTPS header");
        return false;
    }

    // Copy response to buffer
    strncpy(header_buffer, response, buffer_size - 1);
    header_buffer[buffer_size - 1] = '\0';

    // Store in global response buffer (like your original code)
    strncpy(modem_state.global_http_response, response, sizeof(modem_state.global_http_response) - 1);
    modem_state.global_http_response[sizeof(modem_state.global_http_response) - 1] = '\0';

    return true;
}

bool sim7670g_https_get_body(char *body_buffer, size_t buffer_size)
{
    if (!body_buffer || buffer_size == 0) {
        return false;
    }

    // Based on your original modem_https_get_body function
    char response[1024];
    if (!send_at_command("AT+HTTPSDATA", response, 10000)) {
        ESP_LOGE(TAG, "Failed to get HTTPS body");
        return false;
    }

    // Copy response to buffer
    strncpy(body_buffer, response, buffer_size - 1);
    body_buffer[buffer_size - 1] = '\0';

    return true;
}

bool sim7670g_https_end(void)
{
    // Disconnect and terminate HTTPS (based on your original code)
    send_at_command("AT+SHDISC", NULL, 3000);
    return send_at_command("AT+HTTPTERM", NULL, 3000);
}

// Power control functions
esp_err_t sim7670g_power_on(void)
{
    if (modem_state.config.power_pin >= 0) {
        gpio_set_level(modem_state.config.power_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Modem powered on");
    }
    return ESP_OK;
}

esp_err_t sim7670g_power_off(void)
{
    if (modem_state.config.power_pin >= 0) {
        gpio_set_level(modem_state.config.power_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Modem powered off");
    }
    return ESP_OK;
}

esp_err_t sim7670g_reset(void)
{
    if (modem_state.config.reset_pin >= 0) {
        gpio_set_level(modem_state.config.reset_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(modem_state.config.reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Modem reset");
    }
    return ESP_OK;
}

// Private helper functions
static void notify_event(sim7670g_event_t event, int error_code, const char *message)
{
    if (modem_state.event_callback) {
        sim7670g_event_data_t event_data = {
            .event = event,
            .error_code = error_code,
            .message = (char*)message
        };
        modem_state.event_callback(&event_data);
    }
}
/*
static bool wait_for_response(const char *expected, int timeout_ms)
{
    if (!expected) {
        return false;
    }

    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    memset(at_response_buffer, 0, sizeof(at_response_buffer));
    size_t total_read = 0;

    while ((xTaskGetTickCount() - start_time) < timeout_ticks && total_read < (sizeof(at_response_buffer) - 1)) {
        int bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                       (uint8_t*)(at_response_buffer + total_read),
                                       sizeof(at_response_buffer) - total_read - 1,
                                       pdMS_TO_TICKS(100));

        if (bytes_read > 0) {
            total_read += bytes_read;
            at_response_buffer[total_read] = '\0';

            if (strstr(at_response_buffer, expected)) {
                return true;
            }
        }
    }

    return false;
}
*/

static void modem_uart_flush_input(void)
{
    uart_flush_input(modem_state.config.uart_port);
}

//----------------------------------------
// Network time related functions
//----------------------------------------
bool sim7670g_is_initialized(void)
{
    return modem_state.initialized;
}

esp_err_t sim7670g_get_network_time(sim7670g_time_t *network_time)
{
    if (!network_time) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(modem_state.uart_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take UART mutex for network time");
        return ESP_FAIL;
    }

    // Clear response buffer
    memset(at_response_buffer, 0, sizeof(at_response_buffer));

    // Send AT+CCLK? to get network time
    const char *cmd = "AT+CCLK?\r\n";
    uart_flush(modem_state.config.uart_port);
    uart_write_bytes(modem_state.config.uart_port, cmd, strlen(cmd));

    // Wait for response
    esp_err_t result = ESP_FAIL;
    TickType_t start_time = xTaskGetTickCount();
    size_t total_read = 0;

    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(10000) &&
           total_read < (sizeof(at_response_buffer) - 1)) {
        int bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                         (uint8_t*)(at_response_buffer + total_read),
                                         sizeof(at_response_buffer) - total_read - 1,
                                         pdMS_TO_TICKS(100));
        if (bytes_read > 0) {
            total_read += bytes_read;
            at_response_buffer[total_read] = '\0';

            // Look for +CCLK: response
            char *cclk_pos = strstr(at_response_buffer, "+CCLK:");
            if (cclk_pos && strstr(at_response_buffer, "OK")) {
                // Parse time string: +CCLK: "YY/MM/DD,HH:MM:SS±ZZ"
                char time_str[32];
                if (sscanf(cclk_pos, "+CCLK: \"%31[^\"]\"", time_str) == 1) {
                    ESP_LOGI(TAG, "Network time string: %s", time_str);

                    // Parse the time components
                    int year, month, day, hour, minute, second, tz_sign, tz_quarters;
                    char tz_char;

                    if (sscanf(time_str, "%d/%d/%d,%d:%d:%d%c%d",
                              &year, &month, &day, &hour, &minute, &second, &tz_char, &tz_quarters) == 8) {

                        // Convert 2-digit year to 4-digit (assuming 20xx)
                        if (year < 100) {
                            year += 2000;
                        }

                        network_time->year = year;
                        network_time->month = month;
                        network_time->day = day;
                        network_time->hour = hour;
                        network_time->minute = minute;
                        network_time->second = second;
                        network_time->timezone_quarters = (tz_char == '-') ? -tz_quarters : tz_quarters;

                        ESP_LOGI(TAG, "Parsed network time: %04d-%02d-%02d %02d:%02d:%02d (TZ: %+d quarters)",
                                network_time->year, network_time->month, network_time->day,
                                network_time->hour, network_time->minute, network_time->second,
                                network_time->timezone_quarters);

                        result = ESP_OK;
                        break;
                    } else {
                        ESP_LOGE(TAG, "Failed to parse time string: %s", time_str);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to extract time string from response");
                }
                break;
            }
        }
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get network time. Response: %s", at_response_buffer);
    }

    xSemaphoreGive(modem_state.uart_mutex);
    return result;
}

esp_err_t sim7670g_set_rtc_time(const sim7670g_time_t *time_info)
{
    if (!time_info) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(modem_state.uart_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take UART mutex for RTC set");
        return ESP_FAIL;
    }

    // Format: AT+CCLK="YY/MM/DD,HH:MM:SS±ZZ"
    char cmd[64];
    int year_2digit = time_info->year % 100;
    char tz_sign = (time_info->timezone_quarters >= 0) ? '+' : '-';
    int tz_abs = abs(time_info->timezone_quarters);

    snprintf(cmd, sizeof(cmd), "AT+CCLK=\"%02d/%02d/%02d,%02d:%02d:%02d%c%02d\"\r\n",
             year_2digit, time_info->month, time_info->day,
             time_info->hour, time_info->minute, time_info->second,
             tz_sign, tz_abs);

    ESP_LOGI(TAG, "Setting RTC time: %s", cmd);

    // Clear response buffer
    memset(at_response_buffer, 0, sizeof(at_response_buffer));

    uart_flush(modem_state.config.uart_port);
    uart_write_bytes(modem_state.config.uart_port, cmd, strlen(cmd));

    // Wait for OK response
    esp_err_t result = ESP_FAIL;
    TickType_t start_time = xTaskGetTickCount();
    size_t total_read = 0;

    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(5000) &&
           total_read < (sizeof(at_response_buffer) - 1)) {
        int bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                         (uint8_t*)(at_response_buffer + total_read),
                                         sizeof(at_response_buffer) - total_read - 1,
                                         pdMS_TO_TICKS(100));
        if (bytes_read > 0) {
            total_read += bytes_read;
            at_response_buffer[total_read] = '\0';

            if (strstr(at_response_buffer, "OK")) {
                ESP_LOGI(TAG, "RTC time set successfully");
                result = ESP_OK;
                break;
            } else if (strstr(at_response_buffer, "ERROR")) {
                ESP_LOGE(TAG, "Failed to set RTC time");
                break;
            }
        }
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "RTC set timeout or error. Response: %s", at_response_buffer);
    }

    xSemaphoreGive(modem_state.uart_mutex);
    return result;
}

esp_err_t sim7670g_get_rtc_time(sim7670g_time_t *time_info)
{
    // This is the same as get_network_time since AT+CCLK? returns current RTC time
    return sim7670g_get_network_time(time_info);
}

esp_err_t sim7670g_sync_time_from_network(void)
{
    ESP_LOGI(TAG, "Synchronizing RTC with network time...");

    sim7670g_time_t network_time;
    esp_err_t result = sim7670g_get_network_time(&network_time);

    if (result == ESP_OK) {
        result = sim7670g_set_rtc_time(&network_time);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "✅ Time synchronization successful");
        } else {
            ESP_LOGE(TAG, "❌ Failed to set RTC time");
        }
    } else {
        ESP_LOGE(TAG, "❌ Failed to get network time");
    }

    return result;
}

time_t sim7670g_time_to_unix(const sim7670g_time_t *sim_time)
{
    struct tm tm_info = {0};

    tm_info.tm_year = sim_time->year - 1900;  // tm_year is years since 1900
    tm_info.tm_mon = sim_time->month - 1;     // tm_mon is 0-11
    tm_info.tm_mday = sim_time->day;
    tm_info.tm_hour = sim_time->hour;
    tm_info.tm_min = sim_time->minute;
    tm_info.tm_sec = sim_time->second;
    tm_info.tm_isdst = -1;  // Let mktime determine DST

    time_t timestamp = mktime(&tm_info);

    // Adjust for timezone (timezone_quarters is in quarters of an hour)
    timestamp -= (sim_time->timezone_quarters * 15 * 60);

    return timestamp;
}

void unix_to_sim7670g_time(time_t unix_time, sim7670g_time_t *sim_time)
{
    struct tm *tm_info = gmtime(&unix_time);

    sim_time->year = tm_info->tm_year + 1900;
    sim_time->month = tm_info->tm_mon + 1;
    sim_time->day = tm_info->tm_mday;
    sim_time->hour = tm_info->tm_hour;
    sim_time->minute = tm_info->tm_min;
    sim_time->second = tm_info->tm_sec;
    sim_time->timezone_quarters = 0;  // UTC
}

esp_err_t sim7670g_get_time_string(char *buffer, size_t buffer_size, const char *format)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    sim7670g_time_t current_time;
    esp_err_t result = sim7670g_get_rtc_time(&current_time);

    if (result == ESP_OK) {
        time_t unix_time = sim7670g_time_to_unix(&current_time);
        struct tm *tm_info = localtime(&unix_time);

        if (format == NULL) {
            format = "%Y-%m-%d %H:%M:%S";  // Default format
        }

        size_t len = strftime(buffer, buffer_size, format, tm_info);
        if (len == 0) {
            ESP_LOGE(TAG, "Failed to format time string");
            return ESP_FAIL;
        }
    }

    return result;
}
