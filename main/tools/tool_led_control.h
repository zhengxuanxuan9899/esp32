#pragma once

#include "esp_err.h"

/**
 * Initialize LED control tool.
 * Configures GPIO for LED control.
 */
esp_err_t tool_led_control_init(void);

/**
 * Execute LED control command.
 *
 * @param input_json   JSON string with command and optional duration
 * @param output       Output buffer for result text
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_led_control_execute(const char *input_json, char *output, size_t output_size);