/**
 * @file mqtt_handler.c
 * @brief MQTT Communication Handler Implementation
 * 
 * ESP32-C2 optimized MQTT client following Cheeko project flow
 * Wait for WiFi → Connect MQTT → Send Login → Receive Token → Receive Config → Notify UDP
 */

#include "mqtt_handler.h"
#include "esp_log.h"
#include "esp_system.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

#define TAG "MQTT_HANDLER"

// MQTT Configuration (Cheeko settings)
#define MQTT_BROKER_URL     "mqtt://139.59.7.72"
#define MQTT_BROKER_PORT    1883
#define MQTT_CLIENT_ID      "ESP32Client"

// MQTT Topics (Cheeko format)
#define PUBLISH_TOPIC       "user/cheekotoy/xyz0082/thing/event/post"
#define SUBSCRIBE_TOPIC     "user/cheekotoy/xyz0082/thing/command/call"
#define ACK_TOPIC          "user/cheekotoy/xyz0082/thing/command/callAck"
#define DATA_TOPIC         "user/cheekotoy/xyz0082/thing/data/post"

// Device Info (Cheeko format)
#define DEVICE_KEY         "xyz0082"
#define HARDWARE_VERSION   "v2.1.0"
#define SOFTWARE_VERSION   "25.6.4.49"

// MQTT client context
static struct {
    esp_mqtt_client_handle_t client;
    mqtt_state_t state;
    char auth_token[128];
    char udp_host[64];
    uint16_t udp_port;
    uint16_t sample_rate;
    uint8_t channels;
    bool token_received;
    bool config_received;
    bool mqtt_task_done;
    TaskHandle_t udp_task_handle;
    mqtt_command_callback_t cmd_callback;
    mqtt_connection_callback_t conn_callback;
    uint32_t sent_count;
    uint32_t received_count;
    bool initialized;
} mqtt_ctx = {0};

// Forward declarations
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static esp_err_t handle_mqtt_message(const char *topic, const char *data, int data_len);
static esp_err_t send_login_message(void);
static esp_err_t send_data_config_message(void);
static esp_err_t send_ack_message(const char *identifier, int msg_id, int result);

/**
 * @brief Initialize MQTT handler (Cheeko flow)
 */
esp_err_t mqtt_handler_init(void) {
    if (mqtt_ctx.initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "🔧 Initializing MQTT handler (Cheeko flow)");
    
    // Initialize context
    mqtt_ctx.state = MQTT_STATE_DISCONNECTED;
    mqtt_ctx.token_received = false;
    mqtt_ctx.config_received = false;
    mqtt_ctx.mqtt_task_done = false;
    mqtt_ctx.initialized = true;
    
    ESP_LOGI(TAG, "✅ MQTT handler initialized");
    return ESP_OK;
}

/**
 * @brief Start MQTT client (Cheeko flow)
 */
esp_err_t mqtt_handler_start(void) {
    if (!mqtt_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (mqtt_ctx.client) {
        ESP_LOGW(TAG, "MQTT client already started");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "🚀 Starting MQTT client (waiting for WiFi)");
    
    // MQTT client configuration
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .credentials.client_id = MQTT_CLIENT_ID,
        .network.timeout_ms = 10000,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
        .buffer.size = 1024,
        .buffer.out_size = 1024,
    };
    
    mqtt_ctx.client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_ctx.client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    // Register event handler
    esp_err_t ret = esp_mqtt_client_register_event(mqtt_ctx.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(mqtt_ctx.client);
        mqtt_ctx.client = NULL;
        return ret;
    }
    
    // Start the client
    ret = esp_mqtt_client_start(mqtt_ctx.client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(mqtt_ctx.client);
        mqtt_ctx.client = NULL;
        return ret;
    }
    
    mqtt_ctx.state = MQTT_STATE_CONNECTING;
    ESP_LOGI(TAG, "📡 MQTT client started, connecting to broker...");
    return ESP_OK;
}

/**
 * @brief Set UDP task handle for notification (Cheeko flow)
 */
void mqtt_set_udp_task_handle(TaskHandle_t udp_task_handle) {
    mqtt_ctx.udp_task_handle = udp_task_handle;
    ESP_LOGI(TAG, "🔗 UDP task handle registered for notification");
}

/**
 * @brief Get authentication token
 */
const char* mqtt_get_auth_token(void) {
    return mqtt_ctx.auth_token;
}

/**
 * @brief Get UDP configuration
 */
esp_err_t mqtt_get_udp_config(char *host, size_t host_len, uint16_t *port) {
    if (!host || !port) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!mqtt_ctx.config_received) {
        return ESP_ERR_INVALID_STATE;
    }
    
    strncpy(host, mqtt_ctx.udp_host, host_len - 1);
    host[host_len - 1] = '\0';
    *port = mqtt_ctx.udp_port;
    
    return ESP_OK;
}

/**
 * @brief Check if ready for UDP streaming
 */
bool mqtt_is_ready_for_udp(void) {
    return mqtt_ctx.token_received && mqtt_ctx.config_received;
}

/**
 * @brief MQTT event handler (Cheeko flow)
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ MQTT connected!");
            mqtt_ctx.state = MQTT_STATE_CONNECTED;
            
            // Subscribe to command topic
            int msg_id = esp_mqtt_client_subscribe(mqtt_ctx.client, SUBSCRIBE_TOPIC, 1);
            ESP_LOGI(TAG, "📥 Subscribed to: %s (msg_id: %d)", SUBSCRIBE_TOPIC, msg_id);
            
            // Send login message (Cheeko flow step 1)
            send_login_message();
            
            // Send data config message (Cheeko flow step 2)
            send_data_config_message();
            
            if (mqtt_ctx.conn_callback) {
                mqtt_ctx.conn_callback(true);
            }
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "📡 MQTT disconnected");
            mqtt_ctx.state = MQTT_STATE_DISCONNECTED;
            if (mqtt_ctx.conn_callback) {
                mqtt_ctx.conn_callback(false);
            }
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "📥 MQTT subscribed, msg_id=%d", event->msg_id);
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "📤 MQTT published, msg_id=%d", event->msg_id);
            mqtt_ctx.sent_count++;
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "📨 MQTT message received:");
            ESP_LOGI(TAG, "   Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "   Data: %.*s", event->data_len, event->data);
            
            // Handle the message (Cheeko flow)
            handle_mqtt_message(event->topic, event->data, event->data_len);
            mqtt_ctx.received_count++;
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ MQTT error occurred");
            mqtt_ctx.state = MQTT_STATE_ERROR;
            break;
            
        default:
            ESP_LOGD(TAG, "Other MQTT event id: %d", event->event_id);
            break;
    }
}

/**
 * @brief Handle MQTT message (Cheeko protocol)
 */
static esp_err_t handle_mqtt_message(const char *topic, const char *data, int data_len) {
    // Create null-terminated string
    char *message = malloc(data_len + 1);
    if (!message) {
        ESP_LOGE(TAG, "Failed to allocate memory for message");
        return ESP_ERR_NO_MEM;
    }
    memcpy(message, data, data_len);
    message[data_len] = '\0';
    
    // Parse JSON
    cJSON *json = cJSON_Parse(message);
    free(message);
    
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON message");
        return ESP_FAIL;
    }
    
    cJSON *identifier = cJSON_GetObjectItem(json, "identifier");
    if (!identifier || !cJSON_IsString(identifier)) {
        ESP_LOGE(TAG, "Missing or invalid identifier");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    
    const char *id = identifier->valuestring;
    ESP_LOGI(TAG, "🔍 Processing command: %s", id);
    
    if (strcmp(id, "updatetoken") == 0) {
        // Handle token update (Cheeko flow step 3)
        cJSON *input_params = cJSON_GetObjectItem(json, "inputParams");
        cJSON *token = cJSON_GetObjectItem(input_params, "token");
        
        if (token && cJSON_IsString(token)) {
            strncpy(mqtt_ctx.auth_token, token->valuestring, sizeof(mqtt_ctx.auth_token) - 1);
            mqtt_ctx.token_received = true;
            ESP_LOGI(TAG, "🔑 Token updated: %s", mqtt_ctx.auth_token);
            
            // Send ACK
            cJSON *msg_id = cJSON_GetObjectItem(json, "msgId");
            int id_val = msg_id ? msg_id->valueint : 0;
            send_ack_message("updatetoken", id_val, 1);
        }
        
    } else if (strcmp(id, "updateconfig") == 0) {
        // Handle config update (Cheeko flow step 4)
        cJSON *input_params = cJSON_GetObjectItem(json, "inputParams");
        cJSON *udp_host = cJSON_GetObjectItem(input_params, "udp_host");
        cJSON *udp_port = cJSON_GetObjectItem(input_params, "udp_port");
        cJSON *sample_rate = cJSON_GetObjectItem(input_params, "sample_rate");
        cJSON *channels = cJSON_GetObjectItem(input_params, "channels");
        
        if (udp_host && cJSON_IsString(udp_host)) {
            strncpy(mqtt_ctx.udp_host, udp_host->valuestring, sizeof(mqtt_ctx.udp_host) - 1);
        }
        if (udp_port && cJSON_IsNumber(udp_port)) {
            mqtt_ctx.udp_port = udp_port->valueint;
        }
        if (sample_rate && cJSON_IsNumber(sample_rate)) {
            mqtt_ctx.sample_rate = sample_rate->valueint;
        }
        if (channels && cJSON_IsNumber(channels)) {
            mqtt_ctx.channels = channels->valueint;
        }
        
        mqtt_ctx.config_received = true;
        
        ESP_LOGI(TAG, "⚙️ Config updated:");
        ESP_LOGI(TAG, "   UDP: %s:%d", mqtt_ctx.udp_host, mqtt_ctx.udp_port);
        ESP_LOGI(TAG, "   Sample Rate: %d Hz", mqtt_ctx.sample_rate);
        ESP_LOGI(TAG, "   Channels: %d", mqtt_ctx.channels);
        
        // Send ACK
        cJSON *msg_id = cJSON_GetObjectItem(json, "msgId");
        int id_val = msg_id ? msg_id->valueint : 0;
        send_ack_message("updateconfig", id_val, 1);
        
        // Notify UDP task that we're ready (Cheeko flow step 5)
        if (mqtt_ctx.token_received && mqtt_ctx.config_received && mqtt_ctx.udp_task_handle) {
            ESP_LOGI(TAG, "🚀 MQTT setup complete - notifying UDP task");
            xTaskNotifyGive(mqtt_ctx.udp_task_handle);
            mqtt_ctx.mqtt_task_done = true;
        }
        
    } else if (strcmp(id, "audioplay") == 0) {
        // Handle audio playback command
        cJSON *input_params = cJSON_GetObjectItem(json, "inputParams");
        cJSON *url = cJSON_GetObjectItem(input_params, "url");
        
        if (url && cJSON_IsString(url)) {
            ESP_LOGI(TAG, "🎧 Audio playback: %s", url->valuestring);
            
            if (mqtt_ctx.cmd_callback) {
                mqtt_ctx.cmd_callback(id, url->valuestring, strlen(url->valuestring), NULL);
            }
        }
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

/**
 * @brief Send login message (Cheeko flow)
 */
static esp_err_t send_login_message(void) {
    cJSON *json = cJSON_CreateObject();
    cJSON *msg_id = cJSON_CreateNumber(1745127368463);
    cJSON *identifier = cJSON_CreateString("login");
    cJSON *out_params = cJSON_CreateObject();
    cJSON *role = cJSON_CreateNumber(1);
    
    cJSON_AddItemToObject(out_params, "role", role);
    cJSON_AddItemToObject(json, "msgId", msg_id);
    cJSON_AddItemToObject(json, "identifier", identifier);
    cJSON_AddItemToObject(json, "outParams", out_params);
    
    char *json_string = cJSON_Print(json);
    
    int result = esp_mqtt_client_publish(mqtt_ctx.client, PUBLISH_TOPIC, json_string, 0, 1, 0);
    
    ESP_LOGI(TAG, "📤 Sent login message:");
    ESP_LOGI(TAG, "%s", json_string);
    
    free(json_string);
    cJSON_Delete(json);
    
    return (result >= 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Send data config message (Cheeko flow)
 */
static esp_err_t send_data_config_message(void) {
    cJSON *json = cJSON_CreateObject();
    cJSON *msg_id = cJSON_CreateNumber(351969);
    cJSON *identifier = cJSON_CreateString("data_config");
    cJSON *out_params = cJSON_CreateObject();
    
    cJSON_AddItemToObject(out_params, "hardware_ver", cJSON_CreateString(HARDWARE_VERSION));
    cJSON_AddItemToObject(out_params, "software_version", cJSON_CreateString(SOFTWARE_VERSION));
    cJSON_AddItemToObject(out_params, "devicekey", cJSON_CreateString(DEVICE_KEY));
    
    cJSON_AddItemToObject(json, "msgId", msg_id);
    cJSON_AddItemToObject(json, "identifier", identifier);
    cJSON_AddItemToObject(json, "outParams", out_params);
    
    char *json_string = cJSON_Print(json);
    
    int result = esp_mqtt_client_publish(mqtt_ctx.client, DATA_TOPIC, json_string, 0, 1, 0);
    
    ESP_LOGI(TAG, "📤 Sent data_config message:");
    ESP_LOGI(TAG, "%s", json_string);
    
    free(json_string);
    cJSON_Delete(json);
    
    return (result >= 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Send ACK message (Cheeko flow)
 */
static esp_err_t send_ack_message(const char *identifier, int msg_id, int result) {
    cJSON *json = cJSON_CreateObject();
    
    cJSON_AddItemToObject(json, "msgId", cJSON_CreateNumber(msg_id));
    cJSON_AddItemToObject(json, "identifier", cJSON_CreateString(identifier));
    cJSON_AddItemToObject(json, "result", cJSON_CreateNumber(result));
    
    char *json_string = cJSON_Print(json);
    
    int pub_result = esp_mqtt_client_publish(mqtt_ctx.client, ACK_TOPIC, json_string, 0, 1, 0);
    
    ESP_LOGI(TAG, "📤 Sent ACK for %s (result: %d):", identifier, result);
    ESP_LOGI(TAG, "%s", json_string);
    
    free(json_string);
    cJSON_Delete(json);
    
    return (pub_result >= 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Register command callback
 */
void mqtt_register_command_callback(mqtt_command_callback_t callback) {
    mqtt_ctx.cmd_callback = callback;
}

/**
 * @brief Register connection callback
 */
void mqtt_register_connection_callback(mqtt_connection_callback_t callback) {
    mqtt_ctx.conn_callback = callback;
}

/**
 * @brief Check if MQTT is connected
 */
bool mqtt_is_connected(void) {
    return mqtt_ctx.state == MQTT_STATE_CONNECTED;
}

/**
 * @brief Get current MQTT state
 */
mqtt_state_t mqtt_get_state(void) {
    return mqtt_ctx.state;
}

/**
 * @brief Get MQTT statistics
 */
void mqtt_get_statistics(uint32_t *sent_count, uint32_t *received_count, esp_err_t *last_error) {
    if (sent_count) *sent_count = mqtt_ctx.sent_count;
    if (received_count) *received_count = mqtt_ctx.received_count;
    if (last_error) *last_error = ESP_OK; // TODO: Track last error
} 