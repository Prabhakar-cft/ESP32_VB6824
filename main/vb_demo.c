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
#include <arpa/inet.h>  // For htonl, ntohl, ntohs
#include "esp_random.h"  // For UUID generation
#include <inttypes.h>   // For PRIx32 format specifiers
#include "mbedtls/base64.h"

#define TAG "VB_DEMO"
#define WS_URI "ws://192.168.1.155:5000/ws/audio"
#define BUTTON_GPIO GPIO_NUM_3  // Using GPIO3 as button input
#define VOLUME_UP_GPIO GPIO_NUM_5

// WiFi credentials
#define WIFI_SSID "Your_SSID"
#define WIFI_PASS "Your_Password"

// Audio configuration (matches Python client)
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define FRAME_DURATION 20  // ms
#define FRAME_SIZE 320//  // 320 samples for 20ms at 16kHz
#define MAX_FRAME_SIZE 1275  // Maximum Opus frame size
#define READ_TASK_STACK_SIZE 8192

static uint8_t s_volume = 50; // Start at 50%

// Session control
static char session_id[37];  // UUID4 string length + null terminator
OpusCodec *s_opus_codec = NULL;
esp_websocket_client_handle_t ws_client = NULL;
TaskHandle_t read_task_handle = NULL;
static bool ws_connected = false;
static bool playback_enabled = false;

// State management
typedef enum {
    STATE_IDLE = 0,
    STATE_RECORDING,
    STATE_PLAYBACK,
    STATE_ERROR
} app_state_t;

static app_state_t app_state = STATE_IDLE;

// Playback buffer for incoming PCM (only needed if accumulating smaller PCM chunks, not for Opus)
// static int16_t s_pcm_playback_buffer[FRAME_SIZE]; // FRAME_SIZE is 320 samples
// static size_t s_pcm_playback_buffer_idx = 0;      // Current index in samples

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

static void volume_up_task(void *arg) {
    bool last_button_state = false;
    while (1) {
        bool current_button_state = gpio_get_level(VOLUME_UP_GPIO);
        if (!current_button_state && last_button_state) {
            if (s_volume < 100) {
                s_volume += 10;
                if (s_volume > 100) s_volume = 100;
                vb6824_audio_set_output_volume(s_volume);
                ESP_LOGI(TAG, "Volume increased to %d%%", s_volume);
            }
            vTaskDelay(pdMS_TO_TICKS(200)); // Debounce
        }
        last_button_state = current_button_state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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

static void handle_received_base64_opus(const char *base64_str, size_t base64_len) {
    uint8_t opus_data[MAX_FRAME_SIZE];
    size_t opus_len = 0;
    int ret = mbedtls_base64_decode(opus_data, sizeof(opus_data), &opus_len, (const unsigned char *)base64_str, base64_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 decode failed: %d", ret);
        return;
    }
    int16_t pcm_data[FRAME_SIZE] __attribute__((aligned(16)));
    int frame_size = opus_codec_decode(s_opus_codec, opus_data, opus_len, pcm_data);
    if (frame_size > 0) {
        vb6824_audio_write((uint8_t *)pcm_data, frame_size * 2);
        ESP_LOGD(TAG, "Played decoded base64 Opus frame: %d samples", frame_size);
    } else {
        ESP_LOGE(TAG, "Failed to decode base64 Opus data: %d", frame_size);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ws_connected = true;
            ESP_LOGI(TAG, "WebSocket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ws_connected = false;
            ESP_LOGI(TAG, "WebSocket disconnected");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (playback_enabled && data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                // int16_t pcm_data[320];
                // int frame_size = opus_codec_decode(s_opus_codec, (uint8_t *)data->data_ptr, data->data_len, pcm_data);
                // if (frame_size > 0) {
                //     vb6824_audio_write((uint8_t *)pcm_data, frame_size * 2);
                // } else {
                //     ESP_LOGE(TAG, "Failed to decode Opus data from server");
                // }
                handle_received_opus_from_server((const uint8_t *)data->data_ptr, data->data_len);
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
    const char *json_str = "{\"message\": \"start\"}";
    esp_websocket_client_send_text(ws_client, json_str, strlen(json_str), portMAX_DELAY);
    ESP_LOGI(TAG, "Sent: %s", json_str);
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

void set_app_state(app_state_t new_state) {
    if (app_state != new_state) {
        ESP_LOGI(TAG, "State change: %d -> %d", app_state, new_state);
        app_state = new_state;
    }
}

void __read_task(void *arg) {
    uint8_t data[MAX_FRAME_SIZE] __attribute__((aligned(16)));
    while (1) {
        if (app_state == STATE_RECORDING) {
            uint16_t len = vb6824_audio_read(data, sizeof(data));
            if (len > 0) {
                if (ws_connected && ws_client) {
                    uint8_t frame_buffer[MAX_FRAME_SIZE + 4];
                    uint32_t frame_len = htonl(len);
                    memcpy(frame_buffer, &frame_len, 4);
                    memcpy(frame_buffer + 4, data, len);
                    esp_websocket_client_send_bin(ws_client, (const char *)frame_buffer, len + 4, portMAX_DELAY);
                    ESP_LOGD(TAG, "Sent frame: %d bytes", len + 4);
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void button_task(void *arg) {
    bool last_button_state = false;
    while (1) {
        bool current_button_state = gpio_get_level(BUTTON_GPIO);
        if (!current_button_state && last_button_state) {
            playback_enabled = !playback_enabled; // Toggle playback
            ESP_LOGI(TAG, "Playback %s", playback_enabled ? "ENABLED" : "DISABLED");
            if (playback_enabled) {
                send_start_message();
            }
            vTaskDelay(pdMS_TO_TICKS(200)); // Debounce
        }
        last_button_state = current_button_state;
        vTaskDelay(pdMS_TO_TICKS(10));
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

    gpio_config_t vol_io_conf = {
        .pin_bit_mask = (1ULL << VOLUME_UP_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&vol_io_conf);

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

    // Initialize WebSocket client
    esp_websocket_client_config_t ws_cfg = {
        .uri = WS_URI,
        .task_prio = 5,
        .task_stack = 8192,
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

    // Start button task
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    xTaskCreate(volume_up_task, "volume_up_task", 2048, NULL, 5, NULL);
}
