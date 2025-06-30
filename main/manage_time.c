#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "sim7670g_modem.h"
#include "websocket_client.h"  // For websocket_send_text function
#include "modem_tcp_interface.h"  // Add this include
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"         // For esp_timer_get_time()
#include "esp_sntp.h"
#include <sys/time.h>
#include "manage_time.h"

static const char *TIME_TAG = "TIME";

void time_sync_task(void *pvParameters)
{
    ESP_LOGI(TIME_TAG, "=== Time synchronization task started ===");

    // Wait for modem to be ready
    vTaskDelay(pdMS_TO_TICKS(10000));

    // One-time sync at startup
    ESP_LOGI(TIME_TAG, "Performing one-time time synchronization...");
    esp_err_t result = sim7670g_sync_time_from_network();

    if (result == ESP_OK) {
        char time_str[64];
        if (sim7670g_get_time_string(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC") == ESP_OK) {
            ESP_LOGI(TIME_TAG, "✅ Time sync complete: %s", time_str);
        }
    } else {
        ESP_LOGE(TIME_TAG, "❌ Time sync failed");
    }

    ESP_LOGI(TIME_TAG, "Time synchronization complete. RTC will maintain time automatically.");
    ESP_LOGI(TIME_TAG, "Task terminating - no periodic sync needed.");

    // Task terminates - RTC keeps time on its own
    vTaskDelete(NULL);
}


//----------------------------------------
// Timezone name lookup
//----------------------------------------
const char* get_timezone_name(int timezone_quarters)
{
    int hours = timezone_quarters / 4;

    switch (hours) {
        case -8: return "PST";
        case -7: return "PDT/MST";
        case -6: return "MDT/CST";
        case -5: return "CDT/EST";
        case -4: return "EDT";
        case 0:  return "UTC/GMT";
        case 1:  return "CET";
        case 8:  return "CST";
        case 9:  return "JST";
        default:
            static char tz_buffer[16];
            snprintf(tz_buffer, sizeof(tz_buffer), "UTC%+d", hours);
            return tz_buffer;
    }
}


//----------------------------------------
// Function to add timestamp as the FIRST field in JSON
//----------------------------------------
esp_err_t prepend_timestamp_to_json(char *json_buffer, size_t buffer_size)
{
    char time_str[32];
    esp_err_t result = sim7670g_get_time_string(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S");

    if (result == ESP_OK) {
        // Find the opening brace
        char *opening_brace = strchr(json_buffer, '{');
        if (opening_brace) {
            // Calculate insertion point (right after the opening brace)
            char *insert_point = opening_brace + 1;

            // Create timestamp field
            char timestamp_field[64];
            snprintf(timestamp_field, sizeof(timestamp_field), "\"timestamp\":\"%s UTC\",", time_str);

            // Calculate space requirements
            size_t current_len = strlen(json_buffer);
            size_t timestamp_len = strlen(timestamp_field);
            size_t total_needed = current_len + timestamp_len + 1; // +1 for null terminator

            if (total_needed < buffer_size) {
                // Calculate how much content comes after the insertion point
                size_t remaining_content_len = strlen(insert_point);

                // Move existing content to make room for timestamp
                memmove(insert_point + timestamp_len, insert_point, remaining_content_len + 1);

                // Insert the timestamp field
                memcpy(insert_point, timestamp_field, timestamp_len);

                return ESP_OK;
            } else {
                ESP_LOGW("TIME", "Not enough space to prepend timestamp");
                return ESP_ERR_NO_MEM;
            }
        } else {
            ESP_LOGE("TIME", "No opening brace found in JSON");
            return ESP_ERR_INVALID_ARG;
        }
    }

    return result;
}

// Alternative version for different timestamp formats
esp_err_t prepend_timestamp_format(char *json_buffer, size_t buffer_size, const char *format)
{
    char time_str[64];
    esp_err_t result = sim7670g_get_time_string(time_str, sizeof(time_str), format);

    if (result == ESP_OK) {
        char *opening_brace = strchr(json_buffer, '{');
        if (opening_brace) {
            char *insert_point = opening_brace + 1;

            char timestamp_field[96];
            snprintf(timestamp_field, sizeof(timestamp_field), "\"timestamp\":\"%s\",", time_str);

            size_t current_len = strlen(json_buffer);
            size_t timestamp_len = strlen(timestamp_field);

            if (current_len + timestamp_len + 1 < buffer_size) {
                size_t remaining_content_len = strlen(insert_point);
                memmove(insert_point + timestamp_len, insert_point, remaining_content_len + 1);
                memcpy(insert_point, timestamp_field, timestamp_len);
                return ESP_OK;
            } else {
                ESP_LOGW("TIME", "Not enough space to prepend timestamp");
                return ESP_ERR_NO_MEM;
            }
        }
    }
    return result;
}

// Convenience functions for different formats
esp_err_t prepend_simple_timestamp(char *json_buffer, size_t buffer_size)
{
    return prepend_timestamp_format(json_buffer, buffer_size, "%Y-%m-%d %H:%M:%S UTC");
}

esp_err_t prepend_friendly_timestamp(char *json_buffer, size_t buffer_size)
{
    return prepend_timestamp_format(json_buffer, buffer_size, "%b %d, %Y at %I:%M %p");
}

esp_err_t prepend_time_only(char *json_buffer, size_t buffer_size)
{
    return prepend_timestamp_format(json_buffer, buffer_size, "%H:%M:%S");
}
