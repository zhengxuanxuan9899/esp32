#include "tool_led_control.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "led_control";

/* LED GPIO configuration - adjust based on your board */
#define LED_GPIO_NUM    GPIO_NUM_2  /* Common on ESP32-S3 boards */
#define LED_ACTIVE_LOW  false        /* Set true if LED is active-low */

#define LED_TASK_STACK_SIZE 4096
#define LED_TASK_PRIORITY   5
#define LED_TASK_CORE       0

static portMUX_TYPE led_mux = portMUX_INITIALIZER_UNLOCKED;

/* LED state */
typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_ON,
    LED_STATE_BLINKING,
    LED_STATE_PULSING
} led_state_t;

static led_state_t s_led_state = LED_STATE_OFF;
static TaskHandle_t s_led_task_handle = NULL;

/* Blink task */
static void blink_task(void *arg)
{
    uint32_t duration_ms = (uint32_t)(uintptr_t)arg;
    uint32_t period = duration_ms > 0 ? duration_ms : 500;
    uint32_t half_period = period / 2;

    ESP_LOGI(TAG, "Blink task started, period: %d ms", period);

    while (s_led_state == LED_STATE_BLINKING) {
        /* Toggle LED */
        gpio_set_level(LED_GPIO_NUM, LED_ACTIVE_LOW ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(half_period));

        if (s_led_state != LED_STATE_BLINKING) break;

        gpio_set_level(LED_GPIO_NUM, LED_ACTIVE_LOW ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(half_period));
    }

    /* Turn off LED when done */
    gpio_set_level(LED_GPIO_NUM, LED_ACTIVE_LOW ? 0 : 1);

    /**/
    taskENTER_CRITICAL(&led_mux);
    s_led_state = LED_STATE_OFF;
    s_led_task_handle = NULL;
    taskEXIT_CRITICAL(&led_mux);
    vTaskDelete(NULL);
}

/* Pulse task */
static void pulse_task(void *arg)
{
    uint32_t duration_ms = (uint32_t)(uintptr_t)arg;
    uint32_t period = duration_ms > 0 ? duration_ms : 1000;
    uint32_t steps = 20;
    uint32_t step_delay = period / steps;

    ESP_LOGI(TAG, "Pulse task started, duration: %d ms", period);

    /* Fade in */
    for (int i = 0; i <= steps && s_led_state == LED_STATE_PULSING; i++) {
        int level = (LED_ACTIVE_LOW) ? (steps - i) : i;
        gpio_set_level(LED_GPIO_NUM, level > 0 ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }

    /* Fade out */
    for (int i = steps; i >= 0 && s_led_state == LED_STATE_PULSING; i--) {
        int level = (LED_ACTIVE_LOW) ? (steps - i) : i;
        gpio_set_level(LED_GPIO_NUM, level > 0 ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(step_delay));
    }

    /* Turn off LED when done */
    gpio_set_level(LED_GPIO_NUM, LED_ACTIVE_LOW ? 0 : 1);
    taskENTER_CRITICAL(&led_mux);
    s_led_state = LED_STATE_OFF;
    s_led_task_handle = NULL;
    taskEXIT_CRITICAL(&led_mux);
    vTaskDelete(NULL);
}

esp_err_t tool_led_control_init(void)
{
    ESP_LOGI(TAG, "Initializing LED control on GPIO %d", LED_GPIO_NUM);

    /* Configure GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(err));
        return err;
    }

    /* Turn off LED initially */
    gpio_set_level(LED_GPIO_NUM, LED_ACTIVE_LOW ? 0 : 1);
    s_led_state = LED_STATE_OFF;

    ESP_LOGI(TAG, "LED control initialized");

    // 在 tool_led_control_init 函数末尾添加这两行，烧录后看LED是否亮
    gpio_set_level(LED_GPIO_NUM, 1);  // 强制拉高引脚
    ESP_LOGI(TAG, "强制点亮LED，引脚电平：%d", gpio_get_level(LED_GPIO_NUM));

    return ESP_OK;
}

esp_err_t tool_led_control_execute(const char *input_json, char *output, size_t output_size)
{
    /*增加入参校验，避免空指针 */
    if (input_json == NULL || output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // 自动懒加载初始化（首次调用时执行，避免漏调init）
    static bool is_initialized = false;
    if (!is_initialized) {
        esp_err_t init_ret = tool_led_control_init();
        if (init_ret != ESP_OK) {
            snprintf(output, output_size, "LED初始化失败：%s", esp_err_to_name(init_ret));
            return init_ret;
        }
        is_initialized = true;
        ESP_LOGI(TAG, "LED懒加载初始化成功");
    }

    /* Parse input JSON */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    /* Get command */
    cJSON *cmd = cJSON_GetObjectItem(input, "command");
    if (!cmd || !cJSON_IsString(cmd) || cmd->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'command' field. Use: on, off, blink, or pulse");
        return ESP_ERR_INVALID_ARG;
    }

    /* Get optional duration */
    cJSON *dur = cJSON_GetObjectItem(input, "duration");
    uint32_t duration_ms = 0;
    if (dur && cJSON_IsNumber(dur)) {
        duration_ms = (uint32_t)dur->valuedouble;
    }

    /* Stop any existing LED task */
    if (s_led_task_handle != NULL) {
        taskENTER_CRITICAL(&led_mux);
        s_led_state = LED_STATE_OFF;
        taskEXIT_CRITICAL(&led_mux);
        vTaskDelete(s_led_task_handle);
        s_led_task_handle = NULL;
    }

    /* Execute command */
    const char *command = cmd->valuestring;
    esp_err_t result = ESP_OK;

    if (strcmp(command, "on") == 0) {
        s_led_state = LED_STATE_ON;
        gpio_set_level(LED_GPIO_NUM, LED_ACTIVE_LOW ? 0 : 1);
        snprintf(output, output_size, "LED turned on");

    } else if (strcmp(command, "off") == 0) {
        s_led_state = LED_STATE_OFF;
        gpio_set_level(LED_GPIO_NUM, LED_ACTIVE_LOW ? 1 : 0);
        snprintf(output, output_size, "LED turned off");

    } else if (strcmp(command, "blink") == 0) {
        s_led_state = LED_STATE_BLINKING;
        if (xTaskCreatePinnedToCore(
            blink_task, "led_blink",
            LED_TASK_STACK_SIZE, (void *)(uintptr_t)duration_ms,
            LED_TASK_PRIORITY, &s_led_task_handle, 0) != pdPASS) {
            s_led_state = LED_STATE_OFF;
            snprintf(output, output_size, "Error: Failed to start blink task");
            result = ESP_ERR_NO_MEM;
        } else {
            snprintf(output, output_size, "LED is now blinking");
        }

    } else if (strcmp(command, "pulse") == 0) {
        s_led_state = LED_STATE_PULSING;
        if (xTaskCreatePinnedToCore(
            pulse_task, "led_pulse",
            LED_TASK_STACK_SIZE, (void *)(uintptr_t)duration_ms,
            LED_TASK_PRIORITY, &s_led_task_handle, 0) != pdPASS) {
            s_led_state = LED_STATE_OFF;
            snprintf(output, output_size, "Error: Failed to start pulse task");
            result = ESP_ERR_NO_MEM;
        } else {
            snprintf(output, output_size, "LED is now pulsing");
        }

    } else {
        snprintf(output, output_size, "Error: Unknown command '%s'. Use: on, off, blink, or pulse", command);
        result = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(input);
    return result;
}