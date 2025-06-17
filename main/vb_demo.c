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
#include <arpa/inet.h>  // For htonl
#include "esp_random.h"  // For UUID generation
#include <inttypes.h>   // For PRIx32 format specifiers

#define TAG "VB_DEMO"
#define WS_URI "ws://192.168.1.155:5000/ws/audio"
#define BUTTON_GPIO GPIO_NUM_3  // Using GPIO3 as button input

// WiFi credentials
#define WIFI_SSID "PR_Room"
#define WIFI_PASS "Prabha@12345"

// Audio configuration (matches Python client)
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define FRAME_DURATION 20  // ms
#define FRAME_SIZE (SAMPLE_RATE * FRAME_DURATION / 1000)  // 320 samples for 20ms at 16kHz
#define MAX_FRAME_SIZE 1275  // Maximum Opus frame size
#define READ_TASK_STACK_SIZE (8192 * 2)

// Session control
static char session_id[37];  // UUID4 string length + null terminator
OpusCodec *s_opus_codec = NULL;
esp_websocket_client_handle_t ws_client = NULL;
TaskHandle_t read_task_handle = NULL;
bool is_recording = false;
bool ws_connected = false;

// Generate a UUID4 string
static void generate_uuid4(char *uuid_str) {
    uint32_t random[4];
    for (int i = 0; i < 4; i++) {
        random[i] = esp_random();
    }
    
    snprintf(uuid_str, 37, "%08" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%08" PRIx32 "%04" PRIx32,
             random[0],
             (random[1] >> 16) & 0xFFFF,
             random[1] & 0xFFFF,
             (random[2] >> 16) & 0xFFFF,
             random[2] & 0xFFFFFFFF,
             random[3] & 0xFFFF);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
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

static void handle_received_opus(const uint8_t *data, size_t len) {
    if (len < 4) {
        ESP_LOGE(TAG, "Received frame too short");
        return;
    }

    // Extract frame length from header (first 4 bytes)
    uint32_t frame_len;
    memcpy(&frame_len, data, 4);
    frame_len = ntohl(frame_len);  // Convert from network byte order

    if (frame_len > MAX_FRAME_SIZE) {
        ESP_LOGE(TAG, "Received frame too large: %" PRIu32, frame_len);
        return;
    }

    // Decode Opus data to PCM
    int16_t pcm_data[FRAME_SIZE] __attribute__((aligned(16)));
    int frame_size = opus_codec_decode(s_opus_codec, data + 4, frame_len, pcm_data);
    
    if (frame_size > 0) {
        // Play the decoded audio
        vb6824_audio_write((uint8_t *)pcm_data, frame_size * 2);
        ESP_LOGD(TAG, "Played frame: %d samples", frame_size);
    } else {
        ESP_LOGE(TAG, "Failed to decode Opus data: %d", frame_size);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            ws_connected = true;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            ws_connected = false;
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
            ws_connected = false;
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                ESP_LOGI(TAG, "Received text: %.*s", data->data_len, (char *)data->data_ptr);
            } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                handle_received_opus((const uint8_t *)data->data_ptr, data->data_len);
            }
            break;
        default:
            break;
    }
}

void voice_command_callback(char *command, uint16_t len, void *arg)
{
    ESP_LOGI(TAG, "Received voice command: %.*s\n", len, command);
}

static void send_start_message(void) {
    if (!ws_connected) {
        ESP_LOGE(TAG, "Cannot send start message: WebSocket not connected");
        return;
    }
    char json_str[256];
    snprintf(json_str, sizeof(json_str),
             "{\"type\":\"start\",\"session_id\":\"%s\",\"sample_rate\":%d,\"channels\":%d,\"frame_duration\":%d}",
             session_id, SAMPLE_RATE, CHANNELS, FRAME_DURATION);
    esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), portMAX_DELAY);
}

static void send_stop_message(void) {
    if (!ws_connected) {
        ESP_LOGE(TAG, "Cannot send stop message: WebSocket not connected");
        return;
    }
    char json_str[128];
    snprintf(json_str, sizeof(json_str),
             "{\"type\":\"stop\",\"session_id\":\"%s\"}",
             session_id);
    esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), portMAX_DELAY);
}

void __read_task(void *arg) {
    // Align buffers to 16 bytes for better performance
    uint8_t data[MAX_FRAME_SIZE] __attribute__((aligned(16)));
    
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
                    esp_websocket_client_send_bin(ws_client, (const char *)frame_buffer, len + 4, portMAX_DELAY);
                    
                    ESP_LOGD(TAG, "Sent frame: %d bytes", len + 4);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100)); // Avoid busy waiting
        }
    }
}

static void button_task(void *arg) {
    bool last_button_state = false;
    
    while (1) {
        bool current_button_state = gpio_get_level(BUTTON_GPIO);
        
        // Button press detection (active low)
        if (!current_button_state && last_button_state) {
            is_recording = !is_recording;
            
            if (is_recording) {
                ESP_LOGI(TAG, "Starting recording session");
                send_start_message();
            } else {
                ESP_LOGI(TAG, "Stopping recording session");
                send_stop_message();
            }
            
            // Debounce delay
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        
        last_button_state = current_button_state;
        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to prevent busy waiting
    }
}

void app_main(void) {
    // Generate a new UUID4 for this session
    generate_uuid4(session_id);
    ESP_LOGI(TAG, "Generated session ID: %s", session_id);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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
        .task_stack = 4096,
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
        .path = "/ws/audio",
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
