#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket connection states
typedef enum {
    WS_STATE_DISCONNECTED = 0,
    WS_STATE_CONNECTING,
    WS_STATE_CONNECTED,
    WS_STATE_ERROR
} websocket_state_t;

// WebSocket event types
typedef enum {
    WS_EVENT_CONNECTED = 0,
    WS_EVENT_DISCONNECTED,
    WS_EVENT_DATA_RECEIVED,
    WS_EVENT_ERROR,
    WS_EVENT_PING,
    WS_EVENT_PONG
} websocket_event_t;

// WebSocket event data structure
typedef struct {
    websocket_event_t event;
    char *data;
    size_t data_len;
    int error_code;
} websocket_event_data_t;

// WebSocket event callback function type
typedef void (*websocket_event_callback_t)(websocket_event_data_t *event_data);

// WebSocket configuration structure
typedef struct {
    char *server_host;          // e.g., "47.208.219.96"
    int server_port;            // e.g., 8080
    char *path;                 // e.g., "/"
    int reconnect_interval_ms;  // Auto-reconnect interval (0 = disable)
    int ping_interval_ms;       // Heartbeat ping interval (0 = disable)
    int response_timeout_ms;    // Response timeout
    websocket_event_callback_t event_callback;
} websocket_config_t;

/**
 * @brief Initialize WebSocket client
 * 
 * @param config WebSocket configuration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_init(websocket_config_t *config);

/**
 * @brief Connect to WebSocket server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_connect(void);

/**
 * @brief Disconnect from WebSocket server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_disconnect(void);

/**
 * @brief Send text message to WebSocket server
 * 
 * @param message Text message to send
 * @param length Length of message (0 = auto-calculate)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_send_text(const char *message, size_t length);

/**
 * @brief Send binary data to WebSocket server
 * 
 * @param data Binary data to send
 * @param length Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_send_binary(const uint8_t *data, size_t length);

/**
 * @brief Get current WebSocket connection state
 * 
 * @return websocket_state_t Current state
 */
websocket_state_t websocket_client_get_state(void);

/**
 * @brief Check if WebSocket is connected
 * 
 * @return true if connected, false otherwise
 */
bool websocket_client_is_connected(void);

/**
 * @brief Process WebSocket events (call from main loop)
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_process(void);

/**
 * @brief Cleanup WebSocket client resources
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_cleanup(void);

/**
 * @brief Send ping to server (heartbeat)
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t websocket_client_ping(void);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_CLIENT_H
