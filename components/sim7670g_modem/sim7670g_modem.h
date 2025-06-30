#ifndef SIM7670G_MODEM_H
#define SIM7670G_MODEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// SIM card status enumeration
typedef enum {
    SIM_ERROR = 0,
    SIM_LOCKED,
    SIM_READY
} sim_status_t;

// Network registration status
typedef enum {
    REG_UNKNOWN = 0,
    REG_NOT_REGISTERED,
    REG_OK_HOME,
    REG_SEARCHING,
    REG_DENIED,
    REG_OK_ROAMING
} reg_status_t;

// Modem configuration structure
typedef struct {
    int uart_port;              // UART port number
    int tx_pin;                 // TX pin
    int rx_pin;                 // RX pin
    int rts_pin;                // RTS pin (-1 if not used)
    int cts_pin;                // CTS pin (-1 if not used)
    int baud_rate;              // UART baud rate
    int pwrkey_pin;             // Power Key pin (-1 if not used)
    int power_pin;              // Power control pin (-1 if not used)
    int reset_pin;              // Reset pin (-1 if not used)
    const char *apn;            // Default APN
    const char *sim_pin;        // SIM PIN (NULL if not needed)
} sim7670g_config_t;

// Modem status structure
typedef struct {
    bool initialized;
    bool at_responsive;
    sim_status_t sim_status;
    reg_status_t registration_status;
    int16_t signal_quality;
    bool pdp_active;
    char local_ip[16];
    char operator_name[32];
} sim7670g_status_t;

// Event types for callbacks
typedef enum {
    SIM7670G_EVENT_INITIALIZED = 0,
    SIM7670G_EVENT_SIM_READY,
    SIM7670G_EVENT_NETWORK_REGISTERED,
    SIM7670G_EVENT_PDP_ACTIVATED,
    SIM7670G_EVENT_CONNECTION_LOST,
    SIM7670G_EVENT_ERROR
} sim7670g_event_t;

// Event data structure
typedef struct {
    sim7670g_event_t event;
    int error_code;
    char *message;
} sim7670g_event_data_t;

// Event callback function type
typedef void (*sim7670g_event_callback_t)(sim7670g_event_data_t *event_data);

/**
 * @brief Initialize SIM7670G modem
 * 
 * @param config Modem configuration
 * @param event_callback Event callback function (can be NULL)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_init(sim7670g_config_t *config, sim7670g_event_callback_t event_callback);

/**
 * @brief Deinitialize SIM7670G modem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_deinit(void);

/**
 * @brief Test AT command communication
 * 
 * @return true if modem responds to AT commands
 */
bool sim7670g_test_at(void);

/**
 * @brief Get SIM card status
 * 
 * @return sim_status_t Current SIM status
 */
sim_status_t sim7670g_get_sim_status(void);

/**
 * @brief Unlock SIM card with PIN
 * 
 * @param pin SIM PIN code
 * @return true on success
 */
bool sim7670g_sim_unlock(const char *pin);

/**
 * @brief Set network APN
 * 
 * @param apn Access Point Name
 * @return true on success
 */
bool sim7670g_set_apn(const char *apn);

/**
 * @brief Get network registration status
 * 
 * @return reg_status_t Current registration status
 */
reg_status_t sim7670g_get_registration_status(void);

/**
 * @brief Get signal quality
 * 
 * @return int16_t Signal quality (0-31, or 99 if unknown)
 */
int16_t sim7670g_get_signal_quality(void);

/**
 * @brief Activate PDP context (enable data connection)
 * 
 * @return true on success
 */
bool sim7670g_activate_pdp_context(void);

/**
 * @brief Deactivate PDP context
 * 
 * @return true on success
 */
bool sim7670g_deactivate_pdp_context(void);

/**
 * @brief Get local IP address
 * 
 * @param ip_buffer Buffer to store IP address
 * @param buffer_size Size of IP buffer
 * @return true on success
 */
bool sim7670g_get_local_ip(char *ip_buffer, size_t buffer_size);

/**
 * @brief Get operator name
 * 
 * @param operator_buffer Buffer to store operator name
 * @param buffer_size Size of operator buffer
 * @return true on success
 */
bool sim7670g_get_operator(char *operator_buffer, size_t buffer_size);

/**
 * @brief Get modem status
 * 
 * @param status Pointer to status structure to fill
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_get_status(sim7670g_status_t *status);

/**
 * @brief Check if modem is connected to network and ready for data
 * 
 * @return true if ready for data communication
 */
bool sim7670g_is_ready(void);

/**
 * @brief Send raw AT command
 * 
 * @param command AT command to send
 * @param response Buffer for response (can be NULL)
 * @param response_size Size of response buffer
 * @param timeout_ms Timeout in milliseconds
 * @return true on success
 */
bool sim7670g_send_at_command(const char *command, char *response, size_t response_size, int timeout_ms);

/**
 * @brief Power on the modem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_power_on(void);

/**
 * @brief Power off the modem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_power_off(void);

/**
 * @brief Reset the modem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_reset(void);

// TCP/IP Functions

/**
 * @brief Connect to TCP server
 * 
 * @param host Server hostname or IP
 * @param port Server port
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_tcp_connect(const char *host, int port);

/**
 * @brief Disconnect from TCP server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_tcp_disconnect(void);

/**
 * @brief Send data via TCP
 * 
 * @param data Data to send
 * @param length Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_tcp_send(const char *data, size_t length);

/**
 * @brief Receive data via TCP
 * 
 * @param buffer Buffer for received data
 * @param buffer_size Size of buffer
 * @param received_length Actual received length
 * @param timeout_ms Timeout in milliseconds
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sim7670g_tcp_receive(char *buffer, size_t buffer_size, size_t *received_length, int timeout_ms);

/**
 * @brief Check TCP connection status
 * 
 * @return true if TCP connected
 */
bool sim7670g_tcp_is_connected(void);

// HTTP/HTTPS Functions

/**
 * @brief Initialize HTTPS client
 * 
 * @return true on success
 */
bool sim7670g_https_begin(void);

/**
 * @brief Set HTTPS URL
 * 
 * @param url URL to set
 * @return true on success
 */
bool sim7670g_https_set_url(const char *url);

/**
 * @brief Perform HTTPS GET request
 * 
 * @return int HTTP response code
 */
int sim7670g_https_get(void);

/**
 * @brief Get HTTPS response header
 * 
 * @param header_buffer Buffer for header
 * @param buffer_size Size of buffer
 * @return true on success
 */
bool sim7670g_https_get_header(char *header_buffer, size_t buffer_size);

/**
 * @brief Get HTTPS response body
 * 
 * @param body_buffer Buffer for body
 * @param buffer_size Size of buffer
 * @return true on success
 */
bool sim7670g_https_get_body(char *body_buffer, size_t buffer_size);

/**
 * @brief Cleanup HTTPS client
 * 
 * @return true on success
 */
bool sim7670g_https_end(void);

#ifdef __cplusplus
}
#endif

// Time structure for SIM7670G
typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    int timezone_quarters; // Timezone in quarters of an hour (e.g., +8 hours = +32)
} sim7670g_time_t;

/**
 * @brief Check if the modem is initialized
 * @return true if initialized, false otherwise
 */
bool sim7670g_is_initialized(void);

/**
 * @brief Get network time from cellular provider
 * @param network_time Pointer to store the retrieved network time
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sim7670g_get_network_time(sim7670g_time_t *network_time);

/**
 * @brief Set the internal RTC time
 * @param time_info Time structure to set
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sim7670g_set_rtc_time(const sim7670g_time_t *time_info);

/**
 * @brief Get the current RTC time
 * @param time_info Pointer to store the current RTC time
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sim7670g_get_rtc_time(sim7670g_time_t *time_info);

/**
 * @brief Synchronize RTC with network time
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sim7670g_sync_time_from_network(void);

/**
 * @brief Convert SIM7670G time to Unix timestamp
 * @param sim_time SIM7670G time structure
 * @return Unix timestamp (seconds since epoch)
 */
time_t sim7670g_time_to_unix(const sim7670g_time_t *sim_time);

/**
 * @brief Convert Unix timestamp to SIM7670G time
 * @param unix_time Unix timestamp
 * @param sim_time Pointer to store converted time
 */
void unix_to_sim7670g_time(time_t unix_time, sim7670g_time_t *sim_time);

/**
 * @brief Get current time as formatted string
 * @param buffer Buffer to store formatted time string
 * @param buffer_size Size of the buffer
 * @param format Format string (strftime compatible)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sim7670g_get_time_string(char *buffer, size_t buffer_size, const char *format);

#endif // SIM7670G_MODEM_H
