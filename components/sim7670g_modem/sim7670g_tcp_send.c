esp_err_t sim7670g_tcp_send(const char *data, size_t length)
{
    if (!modem_state.tcp_connected || !data || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    char command[64];
    snprintf(command, sizeof(command), "AT+CIPSEND=0,%zu", length);

    // Take mutex for the entire send operation
    if (xSemaphoreTake(modem_state.uart_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take UART mutex for TCP send");
        return ESP_FAIL;
    }

    bool success = false;

    // Clear input buffer
    modem_uart_flush_input();

    // Send CIPSEND command
    char cmd_with_crlf[80];
    snprintf(cmd_with_crlf, sizeof(cmd_with_crlf), "%s\r\n", command);

    int len = uart_write_bytes(modem_state.config.uart_port, cmd_with_crlf, strlen(cmd_with_crlf));
    if (len < 0) {
        ESP_LOGE(TAG, "Failed to send CIPSEND command");
        goto cleanup;
    }

    // Wait for prompt ">"
    TickType_t start_time = xTaskGetTickCount();
    bool got_prompt = false;

    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(3000)) {
        char prompt_buffer[32];
        int bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                       (uint8_t*)prompt_buffer,
                                       sizeof(prompt_buffer) - 1,
                                       pdMS_TO_TICKS(100));

        if (bytes_read > 0) {
            prompt_buffer[bytes_read] = '\0';
            if (strchr(prompt_buffer, '>')) {
                got_prompt = true;
                break;
            }
        }
    }

    if (!got_prompt) {
        ESP_LOGE(TAG, "Did not receive send prompt '>'");
        goto cleanup;
    }

    // Send raw data
    int bytes_written = uart_write_bytes(modem_state.config.uart_port, data, length);
    if (bytes_written != length) {
        ESP_LOGE(TAG, "Failed to send all data bytes");
        goto cleanup;
    }

    // Wait for send confirmation
    memset(at_response_buffer, 0, sizeof(at_response_buffer));
    size_t total_read = 0;
    start_time = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(10000) && total_read < (sizeof(at_response_buffer) - 1)) {
        int bytes_read = uart_read_bytes(modem_state.config.uart_port,
                                       (uint8_t*)(at_response_buffer + total_read),
                                       sizeof(at_response_buffer) - total_read - 1,
                                       pdMS_TO_TICKS(100));

        if (bytes_read > 0) {
            total_read += bytes_read;
            at_response_buffer[total_read] = '\0';

            if (strstr(at_response_buffer, "+CIPSEND: 0,") || strstr(at_response_buffer, "SEND OK")) {
                break;
            }
        }
    }

    success = (strstr(at_response_buffer, "+CIPSEND: 0,") != NULL || strstr(at_response_buffer, "SEND OK") != NULL);
//    if (!success) {
//        ESP_LOGE(TAG, "TCP send failed. Expected '+CIPSEND: 0,' or 'SEND OK', got: %s", at_response_buffer);  // Enhanced this line
//    } else {
//        ESP_LOGD(TAG, "TCP send successful (%zu bytes)", length);
//    }

    if (strstr(at_response_buffer, "+CIPSEND: 0,") != NULL) {
        // Parse the actual sent length from +CIPSEND: 0,sent_bytes
        char *cipsend_pos = strstr(at_response_buffer, "+CIPSEND: 0,");
        if (cipsend_pos) {
            int sent_bytes = atoi(cipsend_pos + 12); // Skip "+CIPSEND: 0,"
            if (sent_bytes == length) {
                success = true;
                ESP_LOGI(TAG, "TCP send successful: %d/%zu bytes", sent_bytes, length);
            } else {
                ESP_LOGE(TAG, "TCP send incomplete: %d/%zu bytes", sent_bytes, length);
            }
        }
    } else if (strstr(at_response_buffer, "SEND OK") != NULL) {
        success = true;
        ESP_LOGI(TAG, "TCP send successful (SEND OK)");
    }

    if (!success) {
        ESP_LOGE(TAG, "TCP send failed. Response: %s", at_response_buffer);
    }

cleanup:
    xSemaphoreGive(modem_state.uart_mutex);
    return success ? ESP_OK : ESP_FAIL;
}

