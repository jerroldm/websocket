#include <stdio.h>
#include <string.h>
#include <nvs_flash.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "sim7670g_modem.h"
#include "websocket_client.h"

#include "config.h"

static const char *TAG = "MAIN";

// Network configuration Defines
#define NETWORK_APN "puffin"
//#define SIM_PIN ""
#define SIM_PIN NULL

// WebSocket server configuration
#define WEBSOCKET_SERVER    "47.208.219.96"
#define WEBSOCKET_PORT      8080
//#define WEBSOCKET_SERVER    "httpbin.org"  // Change from "47.208.219.96"
//#define WEBSOCKET_PORT      80             // Change from 8080

// UART buffer size
#define UART_BUF_SIZE 2048
#define AT_RESPONSE_TIMEOUT_MS 10000

// UART configuration
#define MODEM_UART_PORT         UART_NUM_1
#define CONSOLE_UART_PORT       UART_NUM_0



//----------------------------------------
// SIM7670G event handler
//----------------------------------------
static void sim7670g_event_handler(sim7670g_event_data_t *event_data)
{
    switch (event_data->event) {
        case SIM7670G_EVENT_INITIALIZED:
            ESP_LOGI(TAG, "📡 SIM7670G Initialized");
            break;

        case SIM7670G_EVENT_SIM_READY:
            ESP_LOGI(TAG, "📱 SIM Card Ready");
            break;

        case SIM7670G_EVENT_NETWORK_REGISTERED:
            ESP_LOGI(TAG, "🌐 Network Registered");
            break;

        case SIM7670G_EVENT_PDP_ACTIVATED:
            ESP_LOGI(TAG, "🔗 Data Connection Activated");
            break;

        case SIM7670G_EVENT_CONNECTION_LOST:
            ESP_LOGW(TAG, "❌ Connection Lost");
            break;

        case SIM7670G_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ Modem Error: %d - %s", event_data->error_code,
                    event_data->message ? event_data->message : "Unknown");
            break;
    }
}
//----------------------------------------


//----------------------------------------
// Cellular initialization task
//----------------------------------------
static void cellular_init_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🚀 Starting cellular initialization...");

    // Configure SIM7670G modem
    sim7670g_config_t modem_config = {
        .uart_port = MODEM_UART_PORT,
        .tx_pin = MODEM_TX_PIN,
        .rx_pin = MODEM_RX_PIN,
        .rts_pin = MODEM_RTS_PIN,
        .cts_pin = MODEM_CTS_PIN,
        .baud_rate = MODEM_BAUD_RATE,
        .pwrkey_pin = MODEM_PWRKEY_PIN,
        .power_pin = MODEM_POWER_PIN,
        .reset_pin = MODEM_RESET_PIN,
        .apn = NETWORK_APN,
        .sim_pin = SIM_PIN
    };

    // Initialize modem
    esp_err_t ret = sim7670g_init(&modem_config, sim7670g_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SIM7670G: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    // Wait for modem to respond to AT commands
    ESP_LOGI(TAG, "🔍 Testing AT communication...");
    int at_retry = 0;
    while (!sim7670g_test_at() && at_retry < 30) {
        ESP_LOGI(TAG, ".");
        vTaskDelay(pdMS_TO_TICKS(2000));
        at_retry++;
    }

    if (at_retry >= 30) {
        ESP_LOGE(TAG, "Modem not responding to AT commands");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "✅ Modem AT communication successful");

    // Check SIM card status
    ESP_LOGI(TAG, "📱 Checking SIM card status...");
    sim_status_t sim_status = SIM_ERROR;
    int sim_retry = 0;

    while (sim_status != SIM_READY && sim_retry < 10) {
        sim_status = sim7670g_get_sim_status();

        switch (sim_status) {
            case SIM_READY:
                ESP_LOGI(TAG, "✅ SIM card ready");
                break;
            case SIM_LOCKED:
                ESP_LOGI(TAG, "🔒 SIM card locked, unlocking...");
                if (SIM_PIN) {
                    sim7670g_sim_unlock(SIM_PIN);
                }
                break;
            default:
                ESP_LOGE(TAG, "❌ SIM card error: %d", sim_status);
                break;
        }

        if (sim_status != SIM_READY) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            sim_retry++;
        }
    }

    if (sim_status != SIM_READY) {
        ESP_LOGE(TAG, "SIM card not ready after retries");
        vTaskDelete(NULL);
        return;
    }

    // Set APN
    ESP_LOGI(TAG, "🌐 Setting APN: %s", NETWORK_APN);
    if (!sim7670g_set_apn(NETWORK_APN)) {
        ESP_LOGE(TAG, "Failed to set APN");
        vTaskDelete(NULL);
        return;
    }

    // Wait for network registration
    ESP_LOGI(TAG, "📡 Waiting for network registration...");
    reg_status_t reg_status = REG_UNKNOWN;
    int reg_retry = 0;

    while ((reg_status != REG_OK_HOME && reg_status != REG_OK_ROAMING) && reg_retry < 30) {
        reg_status = sim7670g_get_registration_status();

        if (reg_status == REG_OK_HOME || reg_status == REG_OK_ROAMING) {
            ESP_LOGI(TAG, "✅ Network registered (status: %d)", reg_status);
            break;
        } else if (reg_status == REG_DENIED) {
            ESP_LOGE(TAG, "❌ Network registration denied");
            vTaskDelete(NULL);
            return;
        } else {
            ESP_LOGI(TAG, "⏳ Registration status: %d, signal quality: %d",
                    reg_status, sim7670g_get_signal_quality());
            vTaskDelay(pdMS_TO_TICKS(2000));
            reg_retry++;
        }
    }

    if (reg_status != REG_OK_HOME && reg_status != REG_OK_ROAMING) {
        ESP_LOGE(TAG, "Network registration failed after retries");
        vTaskDelete(NULL);
        return;
    }

    // Activate PDP context
    ESP_LOGI(TAG, "🔗 Activating data connection...");
    if (!sim7670g_activate_pdp_context()) {
        ESP_LOGE(TAG, "Failed to activate PDP context");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000)); // Wait for activation

    // Get local IP address
    char ip_address[32];
    if (sim7670g_get_local_ip(ip_address, sizeof(ip_address))) {
        ESP_LOGI(TAG, "✅ Data connection active, IP: %s", ip_address);
    } else {
        ESP_LOGW(TAG, "Could not get IP address, but continuing");
    }

    // Get operator info
    char operator_name[64];
    if (sim7670g_get_operator(operator_name, sizeof(operator_name))) {
        ESP_LOGI(TAG, "📡 Connected to: %s", operator_name);
    }

    ESP_LOGI(TAG, "🎉 Cellular initialization complete!");

    // Signal that we're ready for WebSocket connection
    // In a real implementation, you might use an event group or notification
    xTaskNotifyGive((TaskHandle_t)pvParameters); // Notify the main task

    vTaskDelete(NULL);
}
//----------------------------------------


//----------------------------------------
// WebSocket event handler
//----------------------------------------
static void websocket_event_handler(websocket_event_data_t *event_data)
{
    switch (event_data->event) {
        case WS_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ WebSocket Connected!");
            // Send initial message
            websocket_client_send_text("ESP32 with SIM7670G connected!", 0);
            break;

        case WS_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "❌ WebSocket Disconnected");
            break;

        case WS_EVENT_DATA_RECEIVED:
            ESP_LOGI(TAG, "📨 Received: %.*s", (int)event_data->data_len, event_data->data);

            // Echo back with ESP32 identifier
            char response[256];
            snprintf(response, sizeof(response), "ESP32 Echo: %.*s",
                    (int)event_data->data_len, event_data->data);
            websocket_client_send_text(response, 0);
            break;

        case WS_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ WebSocket Error: %d", event_data->error_code);
            break;

        case WS_EVENT_PING:
            ESP_LOGD(TAG, "🏓 WebSocket Ping received");
            break;

        case WS_EVENT_PONG:
            ESP_LOGD(TAG, "🏓 WebSocket Pong received");
            break;
    }
}
//----------------------------------------


//----------------------------------------
// WebSocket management task
//----------------------------------------
static void websocket_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🔗 Starting WebSocket task...");

    // Configure WebSocket client
    websocket_config_t ws_config = {
        .server_host = WEBSOCKET_SERVER,
        .server_port = WEBSOCKET_PORT,
        .path = "/",
        .reconnect_interval_ms = 5000,  // Auto-reconnect every 5 seconds
        .ping_interval_ms = 30000,      // Ping every 30 seconds
        .response_timeout_ms = 10000,   // 10 second timeout
        .event_callback = websocket_event_handler
    };

    // Initialize WebSocket client
    esp_err_t ret = websocket_client_init(&ws_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        vTaskDelete(NULL);
        return;
    }

    // Connect to WebSocket server
    ret = websocket_client_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WebSocket server");
        // Don't exit - auto-reconnect will try again
    }

    // Main WebSocket processing loop
    int message_counter = 0;
    while (1) {
        // Process WebSocket events
        websocket_client_process();

        // Send periodic data every 30 seconds
        static uint64_t last_periodic_message = 0;
        uint64_t now = esp_timer_get_time() / 1000000; // Convert to seconds

        if (websocket_client_is_connected() && (now - last_periodic_message) >= 30) {
            // Get current modem status
            sim7670g_status_t modem_status;
            sim7670g_get_status(&modem_status);

            char periodic_msg[400];
            snprintf(periodic_msg, sizeof(periodic_msg), 
                    "{\"type\":\"status\",\"counter\":%d,\"uptime\":%lld,"
                    "\"free_heap\":%lu,\"signal_quality\":%d,\"local_ip\":\"%s\","
                    "\"operator\":\"%s\"}", 
                    ++message_counter, now, esp_get_free_heap_size(),
                    modem_status.signal_quality, modem_status.local_ip,
                    modem_status.operator_name);

            websocket_client_send_text(periodic_msg, 0);
            last_periodic_message = now;
            ESP_LOGI(TAG, "📤 Sent status message #%d", message_counter);
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms loop
    }
}
//----------------------------------------


//----------------------------------------
// Sensor data simulation task
//----------------------------------------
static void sensor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "📊 Starting sensor task...");

    float temperature = 20.0;
    float humidity = 50.0;
    int sensor_reading_count = 0;

    while (1) {
        // Simulate sensor readings
        temperature += ((float)(esp_random() % 20) - 10) / 10.0; // ±1.0°C variation
        humidity += ((float)(esp_random() % 20) - 10) / 5.0;     // ±2.0% variation

        // Keep values in reasonable ranges
        if (temperature < 15.0) temperature = 15.0;
        if (temperature > 35.0) temperature = 35.0;
        if (humidity < 30.0) humidity = 30.0;
        if (humidity > 80.0) humidity = 80.0;

        sensor_reading_count++;

        // Send sensor data if WebSocket is connected
        if (websocket_client_is_connected()) {
            char sensor_data[300];
            snprintf(sensor_data, sizeof(sensor_data),
                    "{\"type\":\"sensor_data\",\"reading\":%d,\"temperature\":%.1f,"
                    "\"humidity\":%.1f,\"timestamp\":%lld}",
                    sensor_reading_count, temperature, humidity,
                    esp_timer_get_time() / 1000000);

            websocket_client_send_text(sensor_data, 0);
            ESP_LOGI(TAG, "📊 Sent sensor data: T=%.1f°C, H=%.1f%%, Reading #%d",
                    temperature, humidity, sensor_reading_count);
        }

        vTaskDelay(pdMS_TO_TICKS(20000)); // Send sensor data every 20 seconds
    }
}
//----------------------------------------


//----------------------------------------
// Main()
//----------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "🚀 %s", BOARD_NAME);

    ESP_LOGI(TAG, "🚀 ESP32 WebSocket Client with SIM7670G Starting...");

    // Initialize NVS (required for some ESP-IDF components)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Get current task handle for notification
    TaskHandle_t main_task = xTaskGetCurrentTaskHandle();

    // Start cellular initialization task
    xTaskCreate(cellular_init_task, "cellular_init", 8192, (void*)main_task, 6, NULL);

    // Wait for cellular initialization to complete
    ESP_LOGI(TAG, "⏳ Waiting for cellular initialization...");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "✅ Cellular ready - starting WebSocket services");

    // Create WebSocket management task
    xTaskCreate(websocket_task, "websocket_task", 8192, NULL, 5, NULL);

    // Create sensor data task
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "🎉 All tasks started - WebSocket client running");

    // Main monitoring loop
    while (1) {
        // Monitor system health
        sim7670g_status_t modem_status;
        esp_err_t status_ret = sim7670g_get_status(&modem_status);

        if (status_ret == ESP_OK) {
            ESP_LOGI(TAG, "📊 System Status:");
            ESP_LOGI(TAG, "   Free Heap: %lu bytes", esp_get_free_heap_size());
            ESP_LOGI(TAG, "   Uptime: %lld seconds", esp_timer_get_time() / 1000000);
            ESP_LOGI(TAG, "   SIM7670G Ready: %s", sim7670g_is_ready() ? "Yes" : "No");
            ESP_LOGI(TAG, "   Signal Quality: %d", modem_status.signal_quality);
            ESP_LOGI(TAG, "   WebSocket Connected: %s", websocket_client_is_connected() ? "Yes" : "No");

            if (strlen(modem_status.local_ip) > 0) {
                ESP_LOGI(TAG, "   Local IP: %s", modem_status.local_ip);
            }
        } else {
            ESP_LOGW(TAG, "Failed to get modem status");
        }

        vTaskDelay(pdMS_TO_TICKS(60000)); // Status update every minute
    }
}
//----------------------------------------
