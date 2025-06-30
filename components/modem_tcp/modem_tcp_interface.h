#ifndef MODEM_TCP_INTERFACE_H
#define MODEM_TCP_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect to a TCP server via cellular modem
 * 
 * @param host Server hostname or IP address
 * @param port Server port number
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modem_tcp_connect(const char *host, int port);

/**
 * @brief Disconnect from TCP server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modem_tcp_disconnect(void);

/**
 * @brief Send data via TCP connection
 * 
 * @param data Data to send
 * @param length Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modem_tcp_send(const char *data, size_t length);

/**
 * @brief Receive data from TCP connection
 * 
 * @param buffer Buffer to store received data
 * @param buffer_size Size of buffer
 * @param received_length Pointer to store actual received length
 * @param timeout_ms Timeout in milliseconds
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT if no data, ESP_ERR_NOT_FOUND if no data available
 */
esp_err_t modem_tcp_receive(char *buffer, size_t buffer_size, size_t *received_length, int timeout_ms);

/**
 * @brief Check if TCP connection is active
 * 
 * @return true if connected, false otherwise
 */
bool modem_tcp_is_connected(void);

/**
 * @brief Get TCP connection status
 * 
 * @param status_buffer Buffer to store status string
 * @param buffer_size Size of status buffer
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modem_tcp_get_status(char *status_buffer, size_t buffer_size);

/**
 * @brief Get current connection information
 * 
 * @param host_buffer Buffer to store host (can be NULL)
 * @param host_buffer_size Size of host buffer
 * @param port Pointer to store port (can be NULL)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t modem_tcp_get_connection_info(char *host_buffer, size_t host_buffer_size, int *port);

#ifdef __cplusplus
}
#endif

#endif // MODEM_TCP_INTERFACE_H
