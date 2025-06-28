#include "led.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define TAG "LED"

/**
 * @brief Initialize RGB LED
 */
esp_err_t led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_GREEN_GPIO) | (1ULL << LED_BLUE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Turn off all LEDs initially
    gpio_set_level(LED_RED_GPIO, 0);
    gpio_set_level(LED_GREEN_GPIO, 0);
    gpio_set_level(LED_BLUE_GPIO, 0);
    
    ESP_LOGI(TAG, "RGB LED initialized on GPIO %d, %d, %d", LED_RED_GPIO, LED_GREEN_GPIO, LED_BLUE_GPIO);
    return ESP_OK;
}

/**
 * @brief Set LED color
 */
esp_err_t led_set_color(led_state_t color) {
    switch (color) {
        case LED_OFF:
            gpio_set_level(LED_RED_GPIO, 0);
            gpio_set_level(LED_GREEN_GPIO, 0);
            gpio_set_level(LED_BLUE_GPIO, 0);
            ESP_LOGD(TAG, "LED set to OFF");
            break;
        case LED_RED:
            gpio_set_level(LED_RED_GPIO, 1);
            gpio_set_level(LED_GREEN_GPIO, 0);
            gpio_set_level(LED_BLUE_GPIO, 0);
            ESP_LOGD(TAG, "LED set to RED");
            break;
        case LED_GREEN:
            gpio_set_level(LED_RED_GPIO, 0);
            gpio_set_level(LED_GREEN_GPIO, 1);
            gpio_set_level(LED_BLUE_GPIO, 0);
            ESP_LOGD(TAG, "LED set to GREEN");
            break;
        case LED_BLUE:
            gpio_set_level(LED_RED_GPIO, 0);
            gpio_set_level(LED_GREEN_GPIO, 0);
            gpio_set_level(LED_BLUE_GPIO, 1);
            ESP_LOGD(TAG, "LED set to BLUE");
            break;
        case LED_YELLOW:
            gpio_set_level(LED_RED_GPIO, 1);
            gpio_set_level(LED_GREEN_GPIO, 1);
            gpio_set_level(LED_BLUE_GPIO, 0);
            ESP_LOGD(TAG, "LED set to YELLOW");
            break;
        case LED_CYAN:
            gpio_set_level(LED_RED_GPIO, 0);
            gpio_set_level(LED_GREEN_GPIO, 1);
            gpio_set_level(LED_BLUE_GPIO, 1);
            ESP_LOGD(TAG, "LED set to CYAN");
            break;
        case LED_MAGENTA:
            gpio_set_level(LED_RED_GPIO, 1);
            gpio_set_level(LED_GREEN_GPIO, 0);
            gpio_set_level(LED_BLUE_GPIO, 1);
            ESP_LOGD(TAG, "LED set to MAGENTA");
            break;
        case LED_WHITE:
            gpio_set_level(LED_RED_GPIO, 1);
            gpio_set_level(LED_GREEN_GPIO, 1);
            gpio_set_level(LED_BLUE_GPIO, 1);
            ESP_LOGD(TAG, "LED set to WHITE");
            break;
        default:
            ESP_LOGE(TAG, "Invalid LED color: %d", color);
            return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * @brief Turn off all LEDs
 */
esp_err_t led_off(void) {
    return led_set_color(LED_OFF);
}

/**
 * @brief Set individual LED states
 */
esp_err_t led_set_rgb(bool red, bool green, bool blue) {
    gpio_set_level(LED_RED_GPIO, red ? 1 : 0);
    gpio_set_level(LED_GREEN_GPIO, green ? 1 : 0);
    gpio_set_level(LED_BLUE_GPIO, blue ? 1 : 0);
    
    ESP_LOGD(TAG, "LED RGB set to R:%d G:%d B:%d", red, green, blue);
    return ESP_OK;
} 