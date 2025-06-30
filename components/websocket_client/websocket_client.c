#include "websocket_client.h"
#include "modem_tcp_interface.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

static const char *TAG = "WEBSOCKET_CLIENT";

// WebSocket GUID for key generation
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// WebSocket opcodes
#define WS_OPCODE_CONTINUATION  0x0
#define WS_OPCODE_TEXT          0x1
#define WS_OPCODE_BINARY        0x2
#define WS_OPCODE_CLOSE         0x8
#define WS_OPCODE_PING          0x9
#define WS_OPCODE_PONG          0xA

// Global WebSocket state
static struct {
    websocket_config_t config;
    websocket_state_t state;
    esp_timer_handle_t ping_timer;
    esp_timer_handle_t reconnect_timer;
    bool initialized;
    char websocket_key[25];  // Base64 encoded key
    char expected_accept[29]; // Expected Sec-WebSocket-Accept
} ws_client = {0};

// Forward declarations
static esp_err_t websocket_perform_handshake(void);
static esp_err_t websocket_send_frame(uint8_t opcode, const uint8_t *payload, size_t length);
static esp_err_t websocket_parse_response(const char *response);
static void websocket_generate_key(void);
static void ping_timer_callback(void *arg);
static void reconnect_timer_callback(void *arg);

esp_err_t websocket_client_init(websocket_config_t *config)
{
    if (!config || !config->server_host || !config->event_callback) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    // Copy configuration
    memset(&ws_client, 0, sizeof(ws_client));
    
    ws_client.config.server_host = strdup(config->server_host);
    ws_client.config.server_port = config->server_port;
    ws_client.config.path = config->path ? strdup(config->path) : strdup("/");
    ws_client.config.reconnect_interval_ms = config->reconnect_interval_ms;
    ws_client.config.ping_interval_ms = config->ping_interval_ms;
    ws_client.config.response_timeout_ms = config->response_timeout_ms > 0 ? config->response_timeout_ms : 10000;
    ws_client.config.event_callback = config->event_callback;
    
    ws_client.state = WS_STATE_DISCONNECTED;
    ws_client.initialized = true;

    // Create timers
    if (ws_client.config.ping_interval_ms > 0) {
        const esp_timer_create_args_t ping_timer_args = {
            .callback = &ping_timer_callback,
            .name = "ws_ping_timer"
        };
        esp_timer_create(&ping_timer_args, &ws_client.ping_timer);
    }

    if (ws_client.config.reconnect_interval_ms > 0) {
        const esp_timer_create_args_t reconnect_timer_args = {
            .callback = &reconnect_timer_callback,
            .name = "ws_reconnect_timer"
        };
        esp_timer_create(&reconnect_timer_args, &ws_client.reconnect_timer);
    }

    ESP_LOGI(TAG, "WebSocket client initialized for %s:%d%s", 
             ws_client.config.server_host, 
             ws_client.config.server_port, 
             ws_client.config.path);

    return ESP_OK;
}

esp_err_t websocket_client_connect(void)
{
    if (!ws_client.initialized) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (ws_client.state == WS_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Already connected");
        return ESP_OK;
    }

    ws_client.state = WS_STATE_CONNECTING;
    ESP_LOGI(TAG, "Connecting to WebSocket server...");

    // Connect TCP socket via modem
    esp_err_t ret = modem_tcp_connect(ws_client.config.server_host, ws_client.config.server_port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TCP connection failed");
        ws_client.state = WS_STATE_ERROR;
        return ret;
    }

    // Perform WebSocket handshake
    ret = websocket_perform_handshake();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket handshake failed");
        modem_tcp_disconnect();
        ws_client.state = WS_STATE_ERROR;
        return ret;
    }

    ws_client.state = WS_STATE_CONNECTED;
    ESP_LOGI(TAG, "WebSocket connected successfully");

    // Start ping timer if configured
    if (ws_client.ping_timer && ws_client.config.ping_interval_ms > 0) {
        esp_timer_start_periodic(ws_client.ping_timer, ws_client.config.ping_interval_ms * 1000);
    }

    // Notify connection
    if (ws_client.config.event_callback) {
        websocket_event_data_t event = {
            .event = WS_EVENT_CONNECTED,
            .data = NULL,
            .data_len = 0,
            .error_code = 0
        };
        ws_client.config.event_callback(&event);
    }

    return ESP_OK;
}

esp_err_t websocket_client_disconnect(void)
{
    if (ws_client.state != WS_STATE_CONNECTED) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Disconnecting WebSocket...");

    // Stop timers
    if (ws_client.ping_timer) {
        esp_timer_stop(ws_client.ping_timer);
    }
    if (ws_client.reconnect_timer) {
        esp_timer_stop(ws_client.reconnect_timer);
    }

    // Send close frame
    websocket_send_frame(WS_OPCODE_CLOSE, NULL, 0);

    // Disconnect TCP
    modem_tcp_disconnect();

    ws_client.state = WS_STATE_DISCONNECTED;

    // Notify disconnection
    if (ws_client.config.event_callback) {
        websocket_event_data_t event = {
            .event = WS_EVENT_DISCONNECTED,
            .data = NULL,
            .data_len = 0,
            .error_code = 0
        };
        ws_client.config.event_callback(&event);
    }

    ESP_LOGI(TAG, "WebSocket disconnected");
    return ESP_OK;
}

esp_err_t websocket_client_send_text(const char *message, size_t length)
{
    if (ws_client.state != WS_STATE_CONNECTED) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!message) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t msg_len = length > 0 ? length : strlen(message);
    ESP_LOGI(TAG, "Sending WebSocket text message (%zu bytes): %s", msg_len, message);  // Add this line
    return websocket_send_frame(WS_OPCODE_TEXT, (const uint8_t *)message, msg_len);
}

esp_err_t websocket_client_send_binary(const uint8_t *data, size_t length)
{
    if (ws_client.state != WS_STATE_CONNECTED) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return websocket_send_frame(WS_OPCODE_BINARY, data, length);
}

websocket_state_t websocket_client_get_state(void)
{
    return ws_client.state;
}

bool websocket_client_is_connected(void)
{
    return ws_client.state == WS_STATE_CONNECTED && modem_tcp_is_connected();
}

esp_err_t websocket_client_process(void)
{
    if (!ws_client.initialized || ws_client.state != WS_STATE_CONNECTED) {
        return ESP_OK;
    }

    // Check if TCP connection is still alive
    if (!modem_tcp_is_connected()) {
        ESP_LOGW(TAG, "TCP connection lost");
        ws_client.state = WS_STATE_DISCONNECTED;
        
        // Start reconnect timer if configured
        if (ws_client.reconnect_timer && ws_client.config.reconnect_interval_ms > 0) {
            esp_timer_start_once(ws_client.reconnect_timer, ws_client.config.reconnect_interval_ms * 1000);
        }
        return ESP_FAIL;
    }

    // Check for incoming data
    static char rx_buffer[1024];
    size_t received_length = 0;
    
    esp_err_t ret = modem_tcp_receive(rx_buffer, sizeof(rx_buffer) - 1, &received_length, 100);
    if (ret == ESP_OK && received_length > 0) {
        rx_buffer[received_length] = '\0';
        
        // Simple frame parsing (you might want to make this more robust)
        if (received_length >= 2) {
            uint8_t opcode = rx_buffer[0] & 0x0F;
//            bool fin = (rx_buffer[0] & 0x80) != 0;
            
            // Extract payload (simplified - doesn't handle masking or extended length)
            size_t payload_start = 2;
            size_t payload_len = rx_buffer[1] & 0x7F;
            
            if (payload_len == 126) {
                payload_start = 4;
                payload_len = (rx_buffer[2] << 8) | rx_buffer[3];
            } else if (payload_len == 127) {
                payload_start = 10;
                // Handle 64-bit length if needed
            }
            
            switch (opcode) {
                case WS_OPCODE_TEXT:
                case WS_OPCODE_BINARY:
                    if (ws_client.config.event_callback && payload_len > 0 && payload_start < received_length) {
                        websocket_event_data_t event = {
                            .event = WS_EVENT_DATA_RECEIVED,
                            .data = &rx_buffer[payload_start],
                            .data_len = payload_len,
                            .error_code = 0
                        };
                        ws_client.config.event_callback(&event);
                    }
                    break;
                    
                case WS_OPCODE_PING:
                    // Respond with pong
                    websocket_send_frame(WS_OPCODE_PONG, (uint8_t*)&rx_buffer[payload_start], payload_len);
                    break;
                    
                case WS_OPCODE_PONG:
                    ESP_LOGD(TAG, "Received pong");
                    break;
                    
                case WS_OPCODE_CLOSE:
                    ESP_LOGI(TAG, "Received close frame");
                    websocket_client_disconnect();
                    break;
            }
        }
    }

    return ESP_OK;
}

esp_err_t websocket_client_ping(void)
{
    if (ws_client.state != WS_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *ping_payload = "ping";
    return websocket_send_frame(WS_OPCODE_PING, (const uint8_t *)ping_payload, strlen(ping_payload));
}

esp_err_t websocket_client_cleanup(void)
{
    if (ws_client.state == WS_STATE_CONNECTED) {
        websocket_client_disconnect();
    }

    // Clean up timers
    if (ws_client.ping_timer) {
        esp_timer_delete(ws_client.ping_timer);
        ws_client.ping_timer = NULL;
    }
    if (ws_client.reconnect_timer) {
        esp_timer_delete(ws_client.reconnect_timer);
        ws_client.reconnect_timer = NULL;
    }

    // Free configuration strings
    if (ws_client.config.server_host) {
        free(ws_client.config.server_host);
    }
    if (ws_client.config.path) {
        free(ws_client.config.path);
    }

    memset(&ws_client, 0, sizeof(ws_client));
    ESP_LOGI(TAG, "WebSocket client cleaned up");

    return ESP_OK;
}

// Private functions

static esp_err_t websocket_perform_handshake(void)
{
    // Generate WebSocket key
    websocket_generate_key();

    // Build handshake request
    char handshake_request[512];
    snprintf(handshake_request, sizeof(handshake_request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        ws_client.config.path,
        ws_client.config.server_host,
        ws_client.config.server_port,
        ws_client.websocket_key);

    ESP_LOGI(TAG, "Sending WebSocket handshake...");
    ESP_LOGI(TAG, "Handshake request:\n%s", handshake_request);  // Add this line

    // Send handshake
    esp_err_t ret = modem_tcp_send(handshake_request, strlen(handshake_request));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send handshake request");
        return ret;
    }

    ESP_LOGI(TAG, "Handshake sent, waiting for response...");  // Add this line

    // Wait for response
    char response[1024];
    size_t response_len = 0;
    ret = modem_tcp_receive(response, sizeof(response) - 1, &response_len, ws_client.config.response_timeout_ms);
    if (ret != ESP_OK || response_len == 0) {
        ESP_LOGE(TAG, "Failed to receive handshake response: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    response[response_len] = '\0';
    ESP_LOGI(TAG, "Handshake response received (%zu bytes):\n%s", response_len, response);  // Add this line

    // Parse response
    return websocket_parse_response(response);
}


static esp_err_t websocket_send_frame(uint8_t opcode, const uint8_t *payload, size_t length)
{
    if (length > 1024) {  // Add reasonable size limit
        ESP_LOGE(TAG, "Payload too large: %zu bytes", length);
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t frame[length + 14]; // Maximum header size is 14 bytes
    size_t frame_len = 0;
    
    // First byte: FIN=1, RSV=000, opcode
    frame[frame_len++] = 0x80 | (opcode & 0x0F);
    
    // Second byte: MASK=1, payload length
    if (length < 126) {
        frame[frame_len++] = 0x80 | (uint8_t)length;
    } else if (length < 65536) {
        frame[frame_len++] = 0x80 | 126;
        frame[frame_len++] = (length >> 8) & 0xFF;
        frame[frame_len++] = length & 0xFF;
    } else {
        frame[frame_len++] = 0x80 | 127;
        // 64-bit length (simplified - only handle lower 32 bits)
        frame[frame_len++] = 0;
        frame[frame_len++] = 0;
        frame[frame_len++] = 0;
        frame[frame_len++] = 0;
        frame[frame_len++] = (length >> 24) & 0xFF;
        frame[frame_len++] = (length >> 16) & 0xFF;
        frame[frame_len++] = (length >> 8) & 0xFF;
        frame[frame_len++] = length & 0xFF;
    }
    
    // Generate a proper random masking key
    uint32_t mask = esp_random();  // Use proper random instead of timer
    uint8_t masking_key[4];
    masking_key[0] = (mask >> 24) & 0xFF;
    masking_key[1] = (mask >> 16) & 0xFF;
    masking_key[2] = (mask >> 8) & 0xFF;
    masking_key[3] = mask & 0xFF;
    
    // Add masking key to frame
    frame[frame_len++] = masking_key[0];
    frame[frame_len++] = masking_key[1];
    frame[frame_len++] = masking_key[2];
    frame[frame_len++] = masking_key[3];
    
    // Debug the frame before masking
    ESP_LOGI(TAG, "WebSocket frame header (%zu bytes):", frame_len);
    ESP_LOG_BUFFER_HEX(TAG, frame, frame_len);
    ESP_LOGI(TAG, "Original payload (%zu bytes): %.*s", length, (int)length, payload);
    ESP_LOGI(TAG, "Masking key: %02X %02X %02X %02X", masking_key[0], masking_key[1], masking_key[2], masking_key[3]);
    
    // Masked payload - fixed masking logic
    for (size_t i = 0; i < length; i++) {
        frame[frame_len++] = payload[i] ^ masking_key[i % 4];
    }
    
    // Debug the complete frame
    ESP_LOGI(TAG, "Complete masked WebSocket frame (%zu bytes):", frame_len);
    ESP_LOG_BUFFER_HEX(TAG, frame, frame_len);
    
    // Send frame via TCP - changed from modem_tcp_send to sim7670g_tcp_send
    esp_err_t result = modem_tcp_send((char*)frame, frame_len);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to send WebSocket frame");
    } else {
        ESP_LOGI(TAG, "✅ Sent masked WebSocket frame successfully");
    }
    
    return result;
}

static esp_err_t websocket_parse_response(const char *response)
{
    // Check status line
    if (!strstr(response, "101 Switching Protocols")) {
        ESP_LOGE(TAG, "Invalid HTTP status in handshake response");
        return ESP_FAIL;
    }

    // Check upgrade header
    if (!strstr(response, "Upgrade: websocket") && !strstr(response, "upgrade: websocket")) {
        ESP_LOGE(TAG, "Missing Upgrade header");
        return ESP_FAIL;
    }

    // Check connection header
    if (!strstr(response, "Connection: Upgrade") && !strstr(response, "connection: upgrade")) {
        ESP_LOGE(TAG, "Missing Connection header");
        return ESP_FAIL;
    }

    // TODO: Verify Sec-WebSocket-Accept header
    // For now, just check if it exists
    if (!strstr(response, "Sec-WebSocket-Accept:") && !strstr(response, "sec-websocket-accept:")) {
        ESP_LOGE(TAG, "Missing Sec-WebSocket-Accept header");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebSocket handshake successful");
    return ESP_OK;
}

static void websocket_generate_key(void)
{
    // Generate a simple random key (in production, use better randomness)
//    uint64_t random_val = esp_timer_get_time();
    snprintf(ws_client.websocket_key, sizeof(ws_client.websocket_key), 
             "x3JJHMbDL1EzLkh9GBhXDw=="); // Simplified - use actual random key
}

static void ping_timer_callback(void *arg)
{
    if (ws_client.state == WS_STATE_CONNECTED) {
        websocket_client_ping();
    }
}

static void reconnect_timer_callback(void *arg)
{
    if (ws_client.state == WS_STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "Attempting to reconnect...");
        websocket_client_connect();
    }
}
