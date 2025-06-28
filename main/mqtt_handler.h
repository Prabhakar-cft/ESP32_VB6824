#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

// MQTT state enumeration
typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR
} mqtt_state_t;

// MQTT command callback function type
typedef void (*mqtt_command_callback_t)(const char *command, const char *data, int data_len, void *user_data);

// MQTT connection callback function type
typedef void (*mqtt_connection_callback_t)(bool connected);

/**
 * @brief Initialize MQTT handler
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_handler_init(void);

/**
 * @brief Start MQTT client
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_handler_start(void);

/**
 * @brief Set UDP task handle for notification
 * @param udp_task_handle Task handle to notify when MQTT setup is complete
 */
void mqtt_set_udp_task_handle(TaskHandle_t udp_task_handle);

/**
 * @brief Get authentication token
 * @return Pointer to auth token string
 */
const char* mqtt_get_auth_token(void);

/**
 * @brief Get UDP configuration
 * @param host Buffer to store UDP host
 * @param host_len Length of host buffer
 * @param port Pointer to store UDP port
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_get_udp_config(char *host, size_t host_len, uint16_t *port);

/**
 * @brief Check if ready for UDP streaming
 * @return true if token and config received, false otherwise
 */
bool mqtt_is_ready_for_udp(void);

/**
 * @brief Register command callback
 * @param callback Function to call when commands are received
 */
void mqtt_register_command_callback(mqtt_command_callback_t callback);

/**
 * @brief Register connection callback
 * @param callback Function to call when connection state changes
 */
void mqtt_register_connection_callback(mqtt_connection_callback_t callback);

/**
 * @brief Check if MQTT is connected
 * @return true if connected, false otherwise
 */
bool mqtt_is_connected(void);

/**
 * @brief Get current MQTT state
 * @return Current MQTT state
 */
mqtt_state_t mqtt_get_state(void);

/**
 * @brief Get MQTT statistics
 * @param sent_count Pointer to store sent message count
 * @param received_count Pointer to store received message count
 * @param last_error Pointer to store last error
 */
void mqtt_get_statistics(uint32_t *sent_count, uint32_t *received_count, esp_err_t *last_error);

#ifdef __cplusplus
}
#endif

#endif // MQTT_HANDLER_H 