#ifndef LED_H
#define LED_H

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// RGB LED GPIO pins
#define LED_RED_GPIO   GPIO_NUM_0
#define LED_GREEN_GPIO GPIO_NUM_1  
#define LED_BLUE_GPIO  GPIO_NUM_2

// LED states
typedef enum {
    LED_OFF = 0,
    LED_RED,
    LED_GREEN, 
    LED_BLUE,
    LED_YELLOW,  // Red + Green
    LED_CYAN,    // Green + Blue
    LED_MAGENTA, // Red + Blue
    LED_WHITE    // All colors
} led_state_t;

/**
 * @brief Initialize RGB LED
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_init(void);

/**
 * @brief Set LED color
 * @param color LED color to set
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_set_color(led_state_t color);

/**
 * @brief Turn off all LEDs
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_off(void);

/**
 * @brief Set individual LED states
 * @param red Red LED state (0=off, 1=on)
 * @param green Green LED state (0=off, 1=on)
 * @param blue Blue LED state (0=off, 1=on)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_set_rgb(bool red, bool green, bool blue);

#ifdef __cplusplus
}
#endif

#endif // LED_H 