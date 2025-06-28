#include <stdio.h>
#include "vb6824.h"
#include "esp_log.h"
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "opus_wapper.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include <arpa/inet.h>   // For htonl
#include "esp_random.h"  // For UUID generation
#include <inttypes.h>    // For PRIx32 format specifiers
#include "mqtt_handler.h"
#include "led.h"         // Add LED include

#define TAG "VB_DEMO"
#define WS_URI "ws://139.59.7.72:5008"
#define BUTTON_GPIO GPIO_NUM_3  // Using GPIO3 as button input

// WiFi credentials
#define WIFI_SSID "craftech360"
#define WIFI_PASS "cftCFT360"

// Audio configuration (matches Python client)
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define FRAME_DURATION 60  // ms (60ms per frame)
#define FRAME_SIZE (SAMPLE_RATE * FRAME_DURATION / 1000)  // 960 samples for 60ms at 16kHz
#define MAX_FRAME_SIZE 1275  // Maximum Opus frame size
#define READ_TASK_STACK_SIZE (8*1024)

// Session control
static char session_id[37];  // UUID4 string length + null terminator
OpusCodec *s_opus_codec = NULL;
esp_websocket_client_handle_t ws_client = NULL;
TaskHandle_t read_task_handle = NULL;
bool is_recording = false;
bool ws_connected = false;

// MQTT integration
static bool mqtt_ready = false;

// Generate a UUID4 string (RFC 4122 compliant)
static void generate_uuid4(char *uuid_str) {
    uint8_t uuid[16];
    for (int i = 0; i < 16; i++) {
        uuid[i] = (uint8_t)esp_random();
    }
    // Set version (4) and variant (10)
    uuid[6] = (uuid[6] & 0x0F) | 0x40; // Version 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80; // Variant 10

    snprintf(uuid_str, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5],
        uuid[6], uuid[7],
        uuid[8], uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
    );
}

// MQTT command callback
static void mqtt_command_callback(const char *command, const char *data, int data_len, void *user_data) {
    ESP_LOGI(TAG, "MQTT Command received: %s", command);
    if (strcmp(command, "audioplay") == 0) {
        ESP_LOGI(TAG, "Audio playback URL: %.*s", data_len, data);
        // TODO: Implement audio playback functionality
    }
}

// MQTT connection callback
static void mqtt_connection_callback(bool connected) {
    if (connected) {
        ESP_LOGI(TAG, "MQTT connected successfully");
    } else {
        ESP_LOGW(TAG, "MQTT disconnected");
        mqtt_ready = false;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi checking...");
        led_set_color(LED_YELLOW);  // Yellow for WiFi checking
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        led_set_color(LED_RED);  // Red for WiFi disconnected
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        led_set_color(LED_GREEN);  // Green for WiFi connected
        
        // Start MQTT client after getting IP
        ESP_LOGI(TAG, "Starting MQTT client...");
        esp_err_t ret = mqtt_handler_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
            led_set_color(LED_RED);  // Red for MQTT failure
        }
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Reverted and modified function to handle Opus from server
static void handle_received_opus_from_server(const uint8_t *data, size_t len) {
    if (len < 4) {
        ESP_LOGE(TAG, "Received Opus frame too short, needs at least 4 bytes for length header");
        return;
    }

    // Extract Opus frame length from header (first 4 bytes, big-endian)
    uint32_t opus_frame_len_from_header;
    memcpy(&opus_frame_len_from_header, data, 4);
    opus_frame_len_from_header = ntohl(opus_frame_len_from_header); // Convert from network byte order

    // Check if the total length matches the header-indicated length plus the header itself
    if (len != (opus_frame_len_from_header + 4)) {
        ESP_LOGW(TAG, "Received Opus frame length mismatch: expected %" PRIu32 " + 4, got %zu", opus_frame_len_from_header, len);
        // We will still attempt to decode using the length from the header
    }

    if (opus_frame_len_from_header > MAX_FRAME_SIZE) {
        ESP_LOGE(TAG, "Received Opus frame data too large: %" PRIu32 ", max is %d", opus_frame_len_from_header, MAX_FRAME_SIZE);
        return;
    }

    // Decode Opus data (which starts after the 4-byte header)
    static int16_t s_pcm_data[FRAME_SIZE] __attribute__((aligned(16)));
    
    int frame_size = opus_codec_decode(s_opus_codec, data + 4, opus_frame_len_from_header, s_pcm_data);
    if (frame_size > 0) {
        vb6824_audio_write((uint8_t *)s_pcm_data, frame_size * 2);
        ESP_LOGD(TAG, "Played decoded Opus frame: %d samples", frame_size);
    } else {
        ESP_LOGE(TAG, "Failed to decode Opus data from server: %d", frame_size);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ WEBSOCKET_EVENT_CONNECTED to %s", WS_URI);
            ws_connected = true;
            led_set_color(LED_CYAN);  // Cyan for WebSocket connected
            vTaskDelay(pdMS_TO_TICKS(1000));
            led_set_color(LED_OFF);  // Back to green
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "❌ WEBSOCKET_EVENT_DISCONNECTED");
            ws_connected = false;
            led_set_color(LED_RED);  // Back to green (WiFi connected, WS disconnected)
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ WEBSOCKET_EVENT_ERROR");
            ws_connected = false;
            led_set_color(LED_RED);  // Red for WebSocket error
            break;
        case WEBSOCKET_EVENT_DATA:
                if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                ESP_LOGI(TAG, "📨 Received server JSON: %.*s", data->data_len, (char *)data->data_ptr);
                
                // Parse JSON to check for stop_stream command
                char json_str[256];
                if (data->data_len < sizeof(json_str) - 1) {
                    memcpy(json_str, data->data_ptr, data->data_len);
                    json_str[data->data_len] = '\0';
                    
                    ESP_LOGI(TAG, "Parsing JSON: %s", json_str);
                    
                    // Check if it's a stop_stream message - try multiple patterns
                    if (strstr(json_str, "stop_stream") != NULL ) {
                        // ESP_LOGI(TAG, "Found stop_stream command - turning LED OFF");
                        led_set_color(LED_OFF);  // Turn off LED for stop_stream}
                        }
                    }
                } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                    // ESP_LOGI(TAG, "🎵 Received binary audio data: %d bytes", data->data_len);
                    led_set_color(LED_BLUE);  // Blue for playing audio
                    // Assume server sends Opus-encoded audio, decode and play
                    handle_received_opus_from_server((const uint8_t *)data->data_ptr, data->data_len);
                }
            break;
        default:
            ESP_LOGD(TAG, "Other WebSocket event: %" PRId32, event_id);
            break;
    }
}

void voice_command_callback(char *command, uint16_t len, void *arg)
{
    ESP_LOGI(TAG, "Received voice command: %.*s\n", len, command);
}

static void send_start_message(void) {
    if (!ws_connected || !ws_client) {
        ESP_LOGE(TAG, "Cannot send start message: WebSocket not connected (connected=%d, client=%p)", ws_connected, ws_client);
        return;
    }
    
    // Get auth token from MQTT
    const char* auth_token = mqtt_get_auth_token();
    if (!auth_token || strlen(auth_token) == 0) {
        ESP_LOGE(TAG, "Cannot send start message: No auth token available");
        return;
    }
    
    char json_str[512];  // Increased buffer size for auth token
    snprintf(json_str, sizeof(json_str),
             "{\"type\":\"start\",\"session_id\":\"%s\",\"sample_rate\":%d,\"channels\":%d,\"frame_duration\":%.1f,\"auth_token\":\"%s\"}",
             session_id, SAMPLE_RATE, CHANNELS, (float)FRAME_DURATION, auth_token);
    
    int result = esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), portMAX_DELAY);
    if (result < 0) {
        ESP_LOGE(TAG, "Failed to send start message: %d", result);
    } else {
        ESP_LOGI(TAG, "📤 Sent start message: %s", json_str);
    }
}

static void send_stop_message(void) {
    if (!ws_connected || !ws_client) {
        ESP_LOGE(TAG, "Cannot send stop message: WebSocket not connected (connected=%d, client=%p)", ws_connected, ws_client);
        return;
    }
    char json_str[128];
    snprintf(json_str, sizeof(json_str),
             "{\"type\":\"stop\",\"session_id\":\"%s\"}",
             session_id);
    
    int result = esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), portMAX_DELAY);
    if (result < 0) {
        ESP_LOGE(TAG, "Failed to send stop message: %d", result);
    } else {
        ESP_LOGI(TAG, "📤 Sent stop message: %s", json_str);
    }
}

void __read_task(void *arg) {
    // Align buffers to 16 bytes for better performance
    uint8_t data[MAX_FRAME_SIZE] __attribute__((aligned(16)));
    // int16_t pcm_data[FRAME_SIZE] __attribute__((aligned(16)));  // Unused for now
    while (1) {
        if (is_recording) {
            uint16_t len = vb6824_audio_read(data, sizeof(data));
            if (len > 0) {
                // Send the Opus data through WebSocket if connected
                if (ws_connected && ws_client) {
                    // Create a buffer for the complete frame (4 bytes header + data)
                    uint8_t frame_buffer[MAX_FRAME_SIZE + 4];
                    // Add frame length header (4 bytes) in big-endian format
                    uint32_t frame_len = htonl(len);
                    memcpy(frame_buffer, &frame_len, 4);
                    // Copy the Opus data after the header
                    memcpy(frame_buffer + 4, data, len);
                    // Send the complete frame
                    int result = esp_websocket_client_send_bin(ws_client, (const char *)frame_buffer, len + 4, portMAX_DELAY);
                    if (result < 0) {
                        ESP_LOGE(TAG, "Failed to send audio frame: %d", result);
                    } else {
                        ESP_LOGD(TAG, "📤 Sent audio frame: %d bytes", len + 4);
                    }
                } else {
                    ESP_LOGW(TAG, "WebSocket not connected, skipping audio frame");
                }
                // Decode for local playback if needed
                // int frame_size = opus_codec_decode(s_opus_codec, data, len, pcm_data);
                // if (frame_size > 0) {
                //     vb6824_audio_write((uint8_t *)pcm_data, frame_size * 2);
                // } else {
                //     ESP_LOGE(TAG, "Failed to decode Opus data: %d", frame_size);
                // }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100)); // Avoid busy waiting
        }
    }
}

static void button_task(void *arg) {
    bool last_button_state = false; // Button not pressed (active high)
    while (1) {
        bool current_button_state = gpio_get_level(BUTTON_GPIO);
        // Button pressed (active high)
        if (current_button_state && !last_button_state) {
            if (!is_recording) {
                // Check if we have auth token from MQTT
                const char* auth_token = mqtt_get_auth_token();
                if (!auth_token || strlen(auth_token) == 0) {
                    ESP_LOGW(TAG, "Button pressed but no auth token available - waiting for MQTT setup");
                    led_set_color(LED_YELLOW);  // Yellow for waiting for auth
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    led_set_color(LED_GREEN);  // Back to green
                } else {
                    is_recording = true;
                    ESP_LOGI(TAG, "Button pressed: Starting recording session");
                    led_set_color(LED_MAGENTA);  // Magenta for recording
                    send_start_message();
                }
            }
        }
        // Button released (active low)
        if (!current_button_state && last_button_state) {
            if (is_recording) {
                is_recording = false;
                ESP_LOGI(TAG, "Button released: Stopping recording session");
                send_stop_message();
                // Return to previous state after recording
                led_set_color(LED_OFF);
            }
        }
        last_button_state = current_button_state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    // Generate a new UUID4 for this session
    generate_uuid4(session_id);
    ESP_LOGI(TAG, "Generated session ID: %s", session_id);

    // Initialize LED
    esp_err_t ret = led_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED: %s", esp_err_to_name(ret));
    } else {
        led_set_color(LED_OFF);  // Start with LED off
    }

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize MQTT handler
    ret = mqtt_handler_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT handler: %s", esp_err_to_name(ret));
        led_set_color(LED_RED);  // Red for MQTT init failure
        return;
    }
    
    // Register MQTT callbacks
    mqtt_register_command_callback(mqtt_command_callback);
    mqtt_register_connection_callback(mqtt_connection_callback);

    // Initialize WiFi
    wifi_init();

    // Configure button GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    s_opus_codec = opus_codec_init(SAMPLE_RATE, SAMPLE_RATE, FRAME_DURATION, CHANNELS);
    if (s_opus_codec == NULL) {
        ESP_LOGE(TAG, "Failed to initialize Opus codec");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to ensure the system is ready
    ESP_LOGI(TAG, "Received voice command: init");
    vb6824_init(GPIO_NUM_10, GPIO_NUM_18);
    vb6824_register_voice_command_cb(voice_command_callback, NULL);
    vb6824_audio_enable_input(1);
    vb6824_audio_enable_output(1);

    // Initialize WebSocket client with improved configuration
    esp_websocket_client_config_t ws_cfg = {
        .uri = WS_URI,
        .task_prio = 5,
        .task_stack = READ_TASK_STACK_SIZE,
        .buffer_size = 1024,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 10000,
        .disable_auto_reconnect = false,
        .skip_cert_common_name_check = true,
        .use_global_ca_store = false,
        .cert_pem = NULL,
        .cert_len = 0,
        .client_cert = NULL,
        .client_cert_len = 0,
        .client_key = NULL,
        .client_key_len = 0,
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
        .subprotocol = NULL,
        .user_agent = NULL,
        .headers = NULL,
        .path = NULL,  // Remove path - connect to root
        .disable_pingpong_discon = false,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 20,
    };
    
    ws_client = esp_websocket_client_init(&ws_cfg);
    if (ws_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
    } else {
        esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
        esp_websocket_client_start(ws_client);
    }

    // Create tasks with increased stack size
    xTaskCreate(__read_task, "read_task", READ_TASK_STACK_SIZE, NULL, 10, &read_task_handle);
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
}