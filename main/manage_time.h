#ifndef MANAGE_TIME_H
#define MANAGE_TIME_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Time synchronization task
 * @param pvParameters Task parameters (unused)
 */
void time_sync_task(void *pvParameters);

/**
 * @brief Prepend timestamp to JSON message
 * @param json_buffer JSON string buffer
 * @param buffer_size Size of the buffer
 * @return ESP_OK on success, error code on failure
 */
esp_err_t prepend_timestamp_to_json(char *json_buffer, size_t buffer_size);
esp_err_t prepend_timestamp_format(char *json_buffer, size_t buffer_size, const char *format);
esp_err_t prepend_simple_timestamp(char *json_buffer, size_t buffer_size);
esp_err_t prepend_friendly_timestamp(char *json_buffer, size_t buffer_size);
esp_err_t prepend_time_only(char *json_buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // MANAGE_TIME_H
