/**
 * bosch-tank: Automated Water Tank Management System
 * ESP32-based automatic filling control for coffee machines with VL6150X/VL6180X-compatible ToF sensor
 * 
 * Main application entry point - Phase 1 Implementation
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// ESP-IDF Core
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_attr.h"

// Storage & Crypto
#include "nvs_flash.h"
#include "nvs.h"

// Hardware
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#if CONFIG_IDF_TARGET_ESP32
#include "driver/touch_sensor_legacy.h"
#endif

// WiFi & Network
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"  // For IP4_ADDR macro
#include "lwip/sockets.h"   // For DNS captive portal server

// Web Server
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

// Network utilities
// #include "mdns.h"  // TODO: Add mdns to CMakeLists.txt REQUIRES
// #include "esp_sntp.h"  // TODO: Add esp_sntp to CMakeLists.txt REQUIRES

// Project Config
#include "config.h"
#include "version.h"

// Task watchdog
#include "esp_task_wdt.h"

// Macro for absolute value
#define ABS(x) ((x) < 0 ? -(x) : (x))

// ============================================================================
// Global Constants & Tags
// ============================================================================

static const char *TAG = "bosch-tank-main";

#define API_VERSION "1.0"
#define I2C_DEVICE_SCL_WAIT_US 8000

// ============================================================================
// Global State
// ============================================================================

typedef struct {
    nvs_handle_t nvs_handle;
    uint32_t threshold_top;
    uint32_t threshold_bottom;
    uint32_t timeout_max;
    uint32_t fill_progress_timeout_ms;
    float flow_rate_l_per_min;  // Liter pro Minute
    uint16_t sensor_distance_cm;
    bool valve_state;  // true = open, false = closed
    bool manual_fill_active;
    bool user_fill_halt;  // Pause auto-fill after explicit STOP
    bool emergency_stop_active;
    char emergency_stop_reason[128];  // Grund für Notstopp
    uint32_t valve_open_count;  // Anzahl Ventilöffnungen
    uint32_t emergency_trigger_count;  // Anzahl Notaus-Ausloesungen
    uint32_t total_open_time_ms;  // Gesamte Öffnungszeit in ms
    float total_liters;  // Gesamte Liter basierend auf Durchfluss
    uint32_t sensor_invalid_read_count;
    uint32_t sensor_fallback_reuse_count;
    uint16_t sensor_last_raw_mm;
    bool sensor_data_stale;
    bool stack_warning_active;
    char stack_warning_message[160];
    uint64_t current_valve_open_start_ms;  // Startzeit der aktuell offenen Ventilsitzung
    uint32_t last_update_timestamp;
} system_state_t;

static system_state_t sys_state = {
    .threshold_top = TANK_THRESHOLD_TOP_DEFAULT,
    .threshold_bottom = TANK_THRESHOLD_BOTTOM_DEFAULT,
    .timeout_max = VALVE_TIMEOUT_MAX_DEFAULT,
    .fill_progress_timeout_ms = FILL_PROGRESS_TIMEOUT_DEFAULT,
    .flow_rate_l_per_min = 10.0f,  // Default 10 L/min
    .sensor_distance_cm = 0,
    .valve_state = false,
    .manual_fill_active = false,
    .user_fill_halt = false,
    .emergency_stop_active = false,
    .emergency_stop_reason = "",
    .valve_open_count = 0,
    .emergency_trigger_count = 0,
    .total_open_time_ms = 0,
    .total_liters = 0.0f,
    .sensor_invalid_read_count = 0,
    .sensor_fallback_reuse_count = 0,
    .sensor_last_raw_mm = 0,
    .sensor_data_stale = false,
    .stack_warning_active = false,
    .stack_warning_message = "",
    .current_valve_open_start_ms = 0,
    .last_update_timestamp = 0
};

// Mutex for sys_state access
static SemaphoreHandle_t sys_state_mutex = NULL;
static SemaphoreHandle_t wifi_state_mutex = NULL;
static SemaphoreHandle_t ota_state_mutex = NULL;

// Task Handles
static TaskHandle_t sensor_task_handle = NULL;
static TaskHandle_t valve_task_handle = NULL;
static TaskHandle_t wifi_task_handle = NULL;
static TaskHandle_t touch_task_handle = NULL;
static TaskHandle_t dns_task_handle = NULL;
static TaskHandle_t stack_monitor_task_handle = NULL;
static TaskHandle_t ota_task_handle = NULL;
static volatile bool dns_server_stop_requested = false;
static volatile bool sensor_reinit_requested = false;

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t vl53l0x_dev_handle = NULL;

#if CONFIG_IDF_TARGET_ESP32
#define TOUCH_KEY_PAD TOUCH_PAD_NUM8
static bool touch_key_enabled = false;
static uint16_t touch_key_baseline = 0;
#endif

// WiFi State Variables
typedef struct {
    bool is_connected;           // WiFi STA connected to network
    bool ap_active;              // AP mode active (fallback)
    char ssid[32];               // Current/last SSID
    uint8_t retry_count;         // WiFi connect retry counter
    uint32_t last_error_code;    // Last WiFi error
    uint32_t last_attempt_tick;  // Timestamp of last connection attempt
} wifi_state_t;

static wifi_state_t wifi_state = {
    .is_connected = false,
    .ap_active = false,
    .ssid = "ESP",
    .retry_count = 0,
    .last_error_code = 0,
    .last_attempt_tick = 0
};

typedef struct {
    bool in_progress;
    bool last_result_ok;
    char phase[24];
    char message[128];
    char last_error[64];
    char current_version[32];
    char target_version[32];
    char url[192];
    uint64_t last_start_ms;
    uint64_t last_end_ms;
} ota_state_t;

static ota_state_t ota_state = {
    .in_progress = false,
    .last_result_ok = false,
    .phase = "IDLE",
    .message = "Kein OTA gestartet",
    .last_error = "",
    .current_version = "",
    .target_version = "",
    .url = "",
    .last_start_ms = 0,
    .last_end_ms = 0,
};

static void get_system_state_snapshot(system_state_t *snapshot);
static void set_manual_fill_active(bool active);
static esp_err_t request_manual_fill(bool enable, const char *source, bool *manual_fill_active_out, const char **message_out);
static void ota_update_task(void *pvParameters);

// ============================================================================
// Phase 1: NVS (Non-Volatile Storage) Initialization
// ============================================================================

/**
 * @brief Initialize NVS with encryption
 */
static esp_err_t init_nvs(void)
{
    ESP_LOGI(TAG, "Initializing NVS...");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition invalid - erasing and reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Open/create NVS namespace
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &sys_state.nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Load thresholds from NVS (or use defaults)
    uint32_t stored_top = 0, stored_bottom = 0, stored_timeout = 0, stored_fill_progress_timeout = 0;
    
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_THRESHOLD_TOP, &stored_top) == ESP_OK) {
        sys_state.threshold_top = stored_top;
        ESP_LOGI(TAG, "Loaded threshold_top from NVS: %d cm", stored_top);
    }
    
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_THRESHOLD_BOTTOM, &stored_bottom) == ESP_OK) {
        sys_state.threshold_bottom = stored_bottom;
        ESP_LOGI(TAG, "Loaded threshold_bottom from NVS: %d cm", stored_bottom);
    }
    
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_VALVE_TIMEOUT_MAX, &stored_timeout) == ESP_OK) {
        sys_state.timeout_max = stored_timeout;
        ESP_LOGI(TAG, "Loaded timeout_max from NVS: %d ms", stored_timeout);
    }

    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_FILL_PROGRESS_TIMEOUT, &stored_fill_progress_timeout) == ESP_OK) {
        sys_state.fill_progress_timeout_ms = stored_fill_progress_timeout;
        ESP_LOGI(TAG, "Loaded fill_progress_timeout_ms from NVS: %d ms", stored_fill_progress_timeout);
    }

    // Load flow rate
    uint32_t stored_flow_rate = 0;
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_FLOW_RATE, &stored_flow_rate) == ESP_OK) {
        sys_state.flow_rate_l_per_min = (float)stored_flow_rate / 100.0f;  // Store as int * 100
        ESP_LOGI(TAG, "Loaded flow_rate_l_per_min from NVS: %.2f L/min", sys_state.flow_rate_l_per_min);
    }

    uint32_t stored_valve_open_count = 0;
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_VALVE_OPEN_COUNT, &stored_valve_open_count) == ESP_OK) {
        sys_state.valve_open_count = stored_valve_open_count;
        ESP_LOGI(TAG, "Loaded valve_open_count from NVS: %lu", (unsigned long)stored_valve_open_count);
    }

    uint32_t stored_emergency_count = 0;
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_EMERGENCY_COUNT, &stored_emergency_count) == ESP_OK) {
        sys_state.emergency_trigger_count = stored_emergency_count;
        ESP_LOGI(TAG, "Loaded emergency_trigger_count from NVS: %lu", (unsigned long)stored_emergency_count);
    }

    uint32_t stored_total_open_time_ms = 0;
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_TOTAL_OPEN_TIME_MS, &stored_total_open_time_ms) == ESP_OK) {
        sys_state.total_open_time_ms = stored_total_open_time_ms;
        ESP_LOGI(TAG, "Loaded total_open_time_ms from NVS: %lu", (unsigned long)stored_total_open_time_ms);
    }

    uint32_t stored_total_liters_centi = 0;
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_TOTAL_LITERS_CENTI, &stored_total_liters_centi) == ESP_OK) {
        sys_state.total_liters = (float)stored_total_liters_centi / 100.0f;
        ESP_LOGI(TAG, "Loaded total_liters from NVS: %.2f L", sys_state.total_liters);
    }

    // Load emergency stop state
    uint32_t stored_emergency = 0;
    if (nvs_get_u32(sys_state.nvs_handle, NVS_KEY_EMERGENCY_STOP, &stored_emergency) == ESP_OK) {
        sys_state.emergency_stop_active = (bool)stored_emergency;
        ESP_LOGI(TAG, "Loaded emergency_stop_active from NVS: %d", sys_state.emergency_stop_active);
    }
    
    ESP_LOGI(TAG, "NVS initialized successfully");
    return ESP_OK;
}

// ============================================================================
// Phase 1: I2C Initialization (for VL6150X/VL6180X TOF sensor)
// ============================================================================

/**
 * @brief Initialize I2C bus for VL6150X/VL6180X-compatible TOF sensor (ESP-IDF v6.0 API)
 */
static esp_err_t init_i2c(void)
{
    ESP_LOGI(TAG, "Initializing I2C bus (ESP-IDF v6.0)...");

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = GPIO_I2C_SDA,
        .scl_io_num = GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        // External 4.7k pull-ups are present on the TOF sensor board,
        // so disable the ESP32 internal I2C pull-ups to avoid conflicts.
        .flags.enable_internal_pullup = false,
        .flags.allow_pd = false,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C master bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOF_SENSOR_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .scl_wait_us = I2C_DEVICE_SCL_WAIT_US,
        .flags.disable_ack_check = false,
    };

    ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &vl53l0x_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TOF device registration failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(i2c_bus_handle);
        i2c_bus_handle = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "✅ I2C initialized on SDA=%d, SCL=%d, scl_wait_us=%d", GPIO_I2C_SDA, GPIO_I2C_SCL, I2C_DEVICE_SCL_WAIT_US);
    return ESP_OK;
}

// ============================================================================
// Phase 1: GPIO Initialization (Valve control + LED)
// ============================================================================

/**
 * @brief Initialize GPIO pins for valve and LED control
 */
static esp_err_t init_gpio(void)
{
    ESP_LOGI(TAG, "Initializing GPIO...");
    
    // Configure Valve control pin (GPIO 32 on Bosch hardware) - OUTPUT
    gpio_config_t valve_cfg = {
        .pin_bit_mask = (1ULL << GPIO_VALVE_CONTROL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    esp_err_t ret = gpio_config(&valve_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure valve GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure LED status pin (GPIO 2) - OUTPUT
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << GPIO_LED_STATUS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    ret = gpio_config(&led_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Brownout protection: Close valve immediately
    gpio_set_level(GPIO_VALVE_CONTROL, 0);  // LOW = closed
    gpio_set_level(GPIO_LED_STATUS, 1);     // HIGH = LED on (status: init)
    
    ESP_LOGI(TAG, "GPIO initialized - Valve on GPIO %d, LED on GPIO %d", 
             GPIO_VALVE_CONTROL, GPIO_LED_STATUS);
    return ESP_OK;
}

#if CONFIG_IDF_TARGET_ESP32
static esp_err_t init_touch_key(void)
{
    ESP_LOGI(TAG, "Initializing touch key on GPIO %d (T8)", GPIO_TOUCH_KEY);

    esp_err_t ret = touch_pad_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_set_fsm_mode failed: %s", esp_err_to_name(ret));
        touch_pad_deinit();
        return ret;
    }

    ret = touch_pad_config(TOUCH_KEY_PAD, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_config failed: %s", esp_err_to_name(ret));
        touch_pad_deinit();
        return ret;
    }

    ret = touch_pad_filter_start(TOUCH_KEY_FILTER_PERIOD_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "touch_pad_filter_start failed: %s", esp_err_to_name(ret));
        touch_pad_deinit();
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(TOUCH_KEY_FILTER_PERIOD_MS * TOUCH_KEY_CALIBRATION_SAMPLES));

    uint32_t baseline_sum = 0;
    uint16_t sample = 0;
    for (int i = 0; i < TOUCH_KEY_CALIBRATION_SAMPLES; i++) {
        ret = touch_pad_read_filtered(TOUCH_KEY_PAD, &sample);
        if (ret != ESP_OK || sample == 0) {
            ESP_LOGE(TAG, "Touch baseline read failed: %s (sample=%u)", esp_err_to_name(ret), sample);
            touch_pad_filter_stop();
            touch_pad_deinit();
            return (ret == ESP_OK) ? ESP_FAIL : ret;
        }
        baseline_sum += sample;
        vTaskDelay(pdMS_TO_TICKS(TOUCH_KEY_FILTER_PERIOD_MS));
    }

    touch_key_baseline = (uint16_t)(baseline_sum / TOUCH_KEY_CALIBRATION_SAMPLES);
    touch_key_enabled = true;

    ESP_LOGI(TAG, "Touch key ready on GPIO %d - baseline=%u threshold<%u%%",
             GPIO_TOUCH_KEY, touch_key_baseline, TOUCH_KEY_THRESHOLD_PERCENT);
    return ESP_OK;
}

static void touch_key_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Touch key task started");

    bool touch_active = false;
    uint8_t touch_samples = 0;
    uint8_t release_samples = 0;

    while (1) {
        uint16_t touch_value = 0;
        esp_err_t ret = touch_pad_read_filtered(TOUCH_KEY_PAD, &touch_value);

        if (ret == ESP_OK && touch_value > 0 && touch_key_baseline > 0) {
            uint16_t threshold = (uint16_t)((touch_key_baseline * TOUCH_KEY_THRESHOLD_PERCENT) / 100U);
            bool touched = touch_value < threshold;

            if (!touch_active && !touched) {
                touch_key_baseline = (uint16_t)(((uint32_t)touch_key_baseline * 7U + touch_value) / 8U);
            }

            if (touched) {
                if (touch_samples < UINT8_MAX) {
                    touch_samples++;
                }
                release_samples = 0;

                if (!touch_active && touch_samples >= TOUCH_KEY_DEBOUNCE_COUNT) {
                    touch_active = true;
                    ESP_LOGI(TAG, "Touch key pressed (value=%u baseline=%u)", touch_value, touch_key_baseline);
                }
            } else {
                if (release_samples < UINT8_MAX) {
                    release_samples++;
                }
                touch_samples = 0;

                if (touch_active && release_samples >= TOUCH_KEY_RELEASE_COUNT) {
                    system_state_t state_snapshot;
                    bool manual_fill_active = false;
                    const char *message = NULL;

                    touch_active = false;
                    get_system_state_snapshot(&state_snapshot);
                    esp_err_t request_err = request_manual_fill(!state_snapshot.manual_fill_active,
                        "Touch key", &manual_fill_active, &message);

                    if (request_err == ESP_OK) {
                        ESP_LOGI(TAG, "Touch key action: %s (manual_fill_active=%d)",
                                 message ? message : "OK", manual_fill_active);
                    } else {
                        ESP_LOGW(TAG, "Touch key ignored: %s",
                                 message ? message : esp_err_to_name(request_err));
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_KEY_SAMPLE_MS));
    }
}
#else
static esp_err_t init_touch_key(void)
{
    ESP_LOGW(TAG, "Touch key not supported on this target");
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

// ============================================================================
// Phase 1: HTTP Server Setup (Forward Declaration)
// ============================================================================
static httpd_handle_t start_webserver(void);
static void dns_server_task(void *pvParameters);

// ============================================================================
// Phase 3: HTTP REST API Handlers
// ============================================================================

/**
 * @brief Helper: Send JSON response
 */
static void send_json_response(httpd_req_t *req, const char *json_data)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_send(req, json_data, strlen(json_data));
}

static void json_escape_string(const char *input, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    if (input == NULL) {
        output[0] = '\0';
        return;
    }

    size_t out_index = 0;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0' && out_index + 1 < output_size;
         cursor++) {
        const char *replacement = NULL;

        switch (*cursor) {
            case '"': replacement = "\\\""; break;
            case '\\': replacement = "\\\\"; break;
            case '\b': replacement = "\\b"; break;
            case '\f': replacement = "\\f"; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default:
                break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (out_index + replacement_len >= output_size) {
                break;
            }
            memcpy(output + out_index, replacement, replacement_len);
            out_index += replacement_len;
            continue;
        }

        if (*cursor < 0x20) {
            output[out_index++] = '?';
            continue;
        }

        output[out_index++] = (char)*cursor;
    }

    output[out_index] = '\0';
}

static esp_err_t receive_request_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len >= (int)buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    int total_read = 0;
    while (total_read < req->content_len) {
        int read_len = httpd_req_recv(req, buffer + total_read, req->content_len - total_read);
        if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (read_len <= 0) {
            return ESP_FAIL;
        }
        total_read += read_len;
    }

    buffer[total_read] = '\0';
    return ESP_OK;
}

static void get_system_state_snapshot(system_state_t *snapshot)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    *snapshot = sys_state;
    xSemaphoreGive(sys_state_mutex);
}

static void get_wifi_state_snapshot(wifi_state_t *snapshot)
{
    xSemaphoreTake(wifi_state_mutex, portMAX_DELAY);
    *snapshot = wifi_state;
    xSemaphoreGive(wifi_state_mutex);
}

static void get_ota_state_snapshot(ota_state_t *snapshot)
{
    if (snapshot == NULL || ota_state_mutex == NULL) {
        return;
    }

    xSemaphoreTake(ota_state_mutex, portMAX_DELAY);
    *snapshot = ota_state;
    xSemaphoreGive(ota_state_mutex);
}

static void collect_cpu_runtime_stats(uint32_t *core0_percent,
                                      uint32_t *core1_percent,
                                      char *top_task,
                                      size_t top_task_size,
                                      uint32_t *top_task_percent,
                                      uint32_t *task_count_out)
{
    if (core0_percent) *core0_percent = 0;
    if (core1_percent) *core1_percent = 0;
    if (top_task_percent) *top_task_percent = 0;
    if (task_count_out) *task_count_out = 0;
    if (top_task && top_task_size > 0) {
        strncpy(top_task, "n/a", top_task_size - 1);
        top_task[top_task_size - 1] = '\0';
    }

#if (configGENERATE_RUN_TIME_STATS == 1)
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        return;
    }

    TaskStatus_t *task_array = (TaskStatus_t *)malloc(task_count * sizeof(TaskStatus_t));
    if (task_array == NULL) {
        return;
    }

    uint32_t total_runtime = 0;
    UBaseType_t actual_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);
    if (task_count_out) {
        *task_count_out = (uint32_t)actual_count;
    }

    if (actual_count > 0 && total_runtime > 0) {
        uint32_t c0 = 0;
        uint32_t c1 = 0;
        uint32_t top_pct = 0;

        for (UBaseType_t i = 0; i < actual_count; i++) {
            uint32_t pct = (task_array[i].ulRunTimeCounter * 100U) / total_runtime;

            if (task_array[i].xCoreID == 0) {
                c0 += pct;
            } else if (task_array[i].xCoreID == 1) {
                c1 += pct;
            }

            if (pct >= top_pct) {
                top_pct = pct;
                if (top_task && top_task_size > 0 && task_array[i].pcTaskName != NULL) {
                    strncpy(top_task, task_array[i].pcTaskName, top_task_size - 1);
                    top_task[top_task_size - 1] = '\0';
                }
            }
        }

        if (c0 > 100U) c0 = 100U;
        if (c1 > 100U) c1 = 100U;

        if (core0_percent) *core0_percent = c0;
        if (core1_percent) *core1_percent = c1;
        if (top_task_percent) *top_task_percent = top_pct;
    }

    free(task_array);
#endif
}

static bool set_stack_warning_message(const char *message)
{
    bool changed = false;

    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    if (!sys_state.stack_warning_active || strcmp(sys_state.stack_warning_message, message) != 0) {
        sys_state.stack_warning_active = true;
        strncpy(sys_state.stack_warning_message, message, sizeof(sys_state.stack_warning_message) - 1);
        sys_state.stack_warning_message[sizeof(sys_state.stack_warning_message) - 1] = '\0';
        changed = true;
    }
    xSemaphoreGive(sys_state_mutex);

    return changed;
}

static bool clear_runtime_warnings(void)
{
    bool changed = false;

    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    if (sys_state.stack_warning_active || sys_state.stack_warning_message[0] != '\0') {
        sys_state.stack_warning_active = false;
        sys_state.stack_warning_message[0] = '\0';
        changed = true;
    }
    xSemaphoreGive(sys_state_mutex);

    return changed;
}

static void check_task_stack_usage(const char *task_name, TaskHandle_t task_handle, uint32_t configured_stack_bytes)
{
    if (task_handle == NULL || configured_stack_bytes == 0) {
        return;
    }

    uint32_t free_stack_bytes = (uint32_t)uxTaskGetStackHighWaterMark(task_handle);
    if (free_stack_bytes > configured_stack_bytes) {
        free_stack_bytes = configured_stack_bytes;
    }

    uint32_t used_stack_bytes = configured_stack_bytes - free_stack_bytes;
    uint32_t used_percent = (used_stack_bytes * 100U) / configured_stack_bytes;

    if (used_percent >= STACK_USAGE_WARNING_PERCENT) {
        char warning_message[160];
        snprintf(warning_message, sizeof(warning_message),
            "Stackwarnung: %s nutzt %lu%% (%lu/%lu Bytes)",
            task_name,
            (unsigned long)used_percent,
            (unsigned long)used_stack_bytes,
            (unsigned long)configured_stack_bytes);

        if (set_stack_warning_message(warning_message)) {
            ESP_LOGE(TAG, "%s", warning_message);
        }
    }
}

static void stack_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Stack monitor task started (warning >= %d%%)", STACK_USAGE_WARNING_PERCENT);

    while (1) {
        check_task_stack_usage("sensor_task", sensor_task_handle, TASK_STACK_SENSOR);
        check_task_stack_usage("valve_task", valve_task_handle, TASK_STACK_VALVE);
        check_task_stack_usage("wifi_task", wifi_task_handle, TASK_STACK_WIFI);
        check_task_stack_usage("dns_task", dns_task_handle, TASK_STACK_WIFI);
#if CONFIG_IDF_TARGET_ESP32
        check_task_stack_usage("touch_task", touch_task_handle, TASK_STACK_TOUCH);
#endif

        vTaskDelay(pdMS_TO_TICKS(TASK_STACK_MONITOR_INTERVAL_MS));
    }
}

static void persist_runtime_counters_locked(void)
{
    if (sys_state.nvs_handle == 0) {
        return;
    }

    esp_err_t err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_VALVE_OPEN_COUNT, sys_state.valve_open_count);
    if (err == ESP_OK) err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_EMERGENCY_COUNT, sys_state.emergency_trigger_count);
    if (err == ESP_OK) err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_TOTAL_OPEN_TIME_MS, sys_state.total_open_time_ms);
    if (err == ESP_OK) err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_TOTAL_LITERS_CENTI, (uint32_t)(sys_state.total_liters * 100.0f));
    if (err == ESP_OK) err = nvs_commit(sys_state.nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist runtime counters: %s", esp_err_to_name(err));
    }
}

static void persist_runtime_counters(void)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    persist_runtime_counters_locked();
    xSemaphoreGive(sys_state_mutex);
}

static void reset_runtime_counters_locked(void)
{
    sys_state.valve_open_count = 0;
    sys_state.emergency_trigger_count = 0;
    sys_state.total_open_time_ms = 0;
    sys_state.total_liters = 0.0f;
    sys_state.sensor_invalid_read_count = 0;
    sys_state.sensor_fallback_reuse_count = 0;
    sys_state.sensor_last_raw_mm = 0;
    sys_state.sensor_data_stale = false;
    persist_runtime_counters_locked();
}

static void reset_runtime_counters(void)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    reset_runtime_counters_locked();
    xSemaphoreGive(sys_state_mutex);
}

static esp_err_t request_manual_fill(bool enable, const char *source, bool *manual_fill_active_out, const char **message_out)
{
    system_state_t state_snapshot;
    const char *message = enable ? "Manual fill started" : "Manual fill stopped";

    get_system_state_snapshot(&state_snapshot);

    if (enable && state_snapshot.emergency_stop_active) {
        if (manual_fill_active_out != NULL) {
            *manual_fill_active_out = state_snapshot.manual_fill_active;
        }
        if (message_out != NULL) {
            *message_out = "Emergency stop active";
        }
        ESP_LOGW(TAG, "%s manual fill blocked: emergency stop active", source);
        return ESP_ERR_INVALID_STATE;
    }

    if (enable && state_snapshot.sensor_distance_cm <= state_snapshot.threshold_top) {
        set_manual_fill_active(false);
        if (manual_fill_active_out != NULL) {
            *manual_fill_active_out = false;
        }
        if (message_out != NULL) {
            *message_out = "Tank already at OBEN threshold";
        }
        ESP_LOGI(TAG, "%s manual fill ignored: tank already full", source);
        return ESP_OK;
    }

    if (enable) {
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        sys_state.user_fill_halt = false;
        xSemaphoreGive(sys_state_mutex);
    }
    set_manual_fill_active(enable);
    if (manual_fill_active_out != NULL) {
        *manual_fill_active_out = enable;
    }
    if (message_out != NULL) {
        *message_out = message;
    }
    ESP_LOGI(TAG, "%s manual fill %s requested", source, enable ? "START" : "STOP");
    return ESP_OK;
}

static void set_manual_fill_active(bool active)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    sys_state.manual_fill_active = active;
    xSemaphoreGive(sys_state_mutex);
}

static void set_valve_and_manual_state(bool valve_open, bool manual_fill_active)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    sys_state.valve_state = valve_open;
    sys_state.manual_fill_active = manual_fill_active;
    xSemaphoreGive(sys_state_mutex);
}

static void begin_valve_session(uint64_t start_time_ms, bool manual_fill_active)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    sys_state.current_valve_open_start_ms = start_time_ms;
    sys_state.valve_open_count++;
    sys_state.valve_state = true;
    sys_state.manual_fill_active = manual_fill_active;
    persist_runtime_counters_locked();
    xSemaphoreGive(sys_state_mutex);
}

static void update_runtime_config(uint32_t top, uint32_t bottom, uint32_t timeout, uint32_t fill_progress_timeout, float flow_rate)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    sys_state.threshold_top = top;
    sys_state.threshold_bottom = bottom;
    sys_state.timeout_max = timeout;
    sys_state.fill_progress_timeout_ms = fill_progress_timeout;
    sys_state.flow_rate_l_per_min = flow_rate;
    xSemaphoreGive(sys_state_mutex);
}

static void record_wifi_disconnect(uint8_t reason, uint32_t now_ms)
{
    xSemaphoreTake(wifi_state_mutex, portMAX_DELAY);
    wifi_state.is_connected = false;
    wifi_state.retry_count++;
    wifi_state.last_error_code = reason;
    wifi_state.last_attempt_tick = now_ms;
    xSemaphoreGive(wifi_state_mutex);
}

static void mark_wifi_connected(void)
{
    xSemaphoreTake(wifi_state_mutex, portMAX_DELAY);
    wifi_state.is_connected = true;
    wifi_state.retry_count = 0;
    wifi_state.ap_active = false;
    xSemaphoreGive(wifi_state_mutex);
}

static void update_wifi_credentials_state(const char *ssid)
{
    xSemaphoreTake(wifi_state_mutex, portMAX_DELAY);
    strncpy(wifi_state.ssid, ssid, sizeof(wifi_state.ssid) - 1);
    wifi_state.ssid[sizeof(wifi_state.ssid) - 1] = '\0';
    wifi_state.retry_count = 0;
    wifi_state.last_attempt_tick = 0;
    wifi_state.is_connected = false;
    wifi_state.ap_active = false;
    xSemaphoreGive(wifi_state_mutex);
}

static void set_wifi_retry_attempt_timestamp(uint32_t now_ms)
{
    xSemaphoreTake(wifi_state_mutex, portMAX_DELAY);
    wifi_state.last_attempt_tick = now_ms;
    xSemaphoreGive(wifi_state_mutex);
}

static void set_wifi_ap_active(bool active)
{
    xSemaphoreTake(wifi_state_mutex, portMAX_DELAY);
    wifi_state.ap_active = active;
    if (active) {
        wifi_state.is_connected = false;
    }
    xSemaphoreGive(wifi_state_mutex);
}

static esp_err_t start_dns_server_task(void)
{
    if (dns_task_handle != NULL) {
        return ESP_OK;
    }

    dns_server_stop_requested = false;
    BaseType_t task_result = xTaskCreatePinnedToCore(
        dns_server_task,
        "dns_task",
        TASK_STACK_WIFI,
        NULL,
        TASK_PRIO_WIFI,
        &dns_task_handle,
        TASK_CORE_NETWORK);
    if (task_result != pdPASS) {
        dns_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void stop_dns_server_task(void)
{
    if (dns_task_handle == NULL) {
        dns_server_stop_requested = false;
        return;
    }

    dns_server_stop_requested = true;
}

static esp_err_t set_fallback_ap_enabled(bool enabled)
{
    wifi_mode_t current_mode;
    esp_err_t err = esp_wifi_get_mode(&current_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WiFi mode: %s", esp_err_to_name(err));
        return err;
    }

    wifi_mode_t target_mode = enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    if (current_mode == target_mode) {
        if (enabled) {
            set_wifi_ap_active(true);
            if (start_dns_server_task() != ESP_OK) {
                ESP_LOGW(TAG, "DNS fallback task could not be started");
            }
        } else {
            stop_dns_server_task();
            set_wifi_ap_active(false);
        }
        return ESP_OK;
    }

    if (!enabled) {
        stop_dns_server_task();
    }

    err = esp_wifi_set_mode(target_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch WiFi mode to %s: %s",
            enabled ? "APSTA" : "STA", esp_err_to_name(err));
        return err;
    }

    set_wifi_ap_active(enabled);

    if (enabled) {
        ESP_LOGW(TAG, "Fallback AP enabled: %s @ " AP_MODE_IP_ADDR, WIFI_SSID_AP_MODE);
        if (start_dns_server_task() != ESP_OK) {
            ESP_LOGW(TAG, "DNS fallback task could not be started");
        }
    } else {
        ESP_LOGI(TAG, "Fallback AP disabled - STA only");
    }

    return ESP_OK;
}

static bool parse_json_int_field(const char *json, const char *key, int *value)
{
    const char *match = strstr(json, key);
    if (match == NULL) {
        return false;
    }

    const char *separator = strchr(match, ':');
    if (separator == NULL) {
        return false;
    }

    *value = atoi(separator + 1);
    return true;
}

static bool parse_json_float_field(const char *json, const char *key, float *value)
{
    const char *match = strstr(json, key);
    if (match == NULL) {
        return false;
    }

    const char *separator = strchr(match, ':');
    if (separator == NULL) {
        return false;
    }

    *value = atof(separator + 1);
    return true;
}

static bool parse_json_string_field(const char *json, const char *key, char *out, size_t out_size)
{
    const char *match = strstr(json, key);
    if (match == NULL || out_size == 0) {
        return false;
    }

    const char *start = strchr(match, ':');
    if (start == NULL) {
        return false;
    }

    start++;
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    if (*start != '"') {
        return false;
    }

    start++;

    size_t out_index = 0;
    while (*start != '\0') {
        if (*start == '"') {
            out[out_index] = '\0';
            return true;
        }

        char current = *start;
        if (current == '\\') {
            start++;
            if (*start == '\0') {
                return false;
            }

            switch (*start) {
                case '"': current = '"'; break;
                case '\\': current = '\\'; break;
                case '/': current = '/'; break;
                case 'b': current = '\b'; break;
                case 'f': current = '\f'; break;
                case 'n': current = '\n'; break;
                case 'r': current = '\r'; break;
                case 't': current = '\t'; break;
                default: current = *start; break;
            }
        }

        if (out_index + 1 >= out_size) {
            return false;
        }

        out[out_index++] = current;
        start++;
    }

    return true;
}

static int calculate_fill_percent(uint16_t sensor_distance_cm, uint32_t threshold_top, uint32_t threshold_bottom)
{
    if (threshold_bottom <= threshold_top) {
        return 0;
    }

    if (sensor_distance_cm <= threshold_top) {
        return 100;
    }

    if (sensor_distance_cm >= threshold_bottom) {
        return 0;
    }

    uint32_t range = threshold_bottom - threshold_top;
    uint32_t current = sensor_distance_cm - threshold_top;
    int percent = (int)(((range - current) * 100U) / range);

    if (percent < 0) {
        return 0;
    }

    if (percent > 100) {
        return 100;
    }

    return percent;
}

static void trigger_emergency_stop(const char *reason)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    bool was_active = sys_state.emergency_stop_active;

    sys_state.emergency_stop_active = true;
    if (reason != NULL) {
        strncpy(sys_state.emergency_stop_reason, reason, sizeof(sys_state.emergency_stop_reason) - 1);
        sys_state.emergency_stop_reason[sizeof(sys_state.emergency_stop_reason) - 1] = '\0';
    }

    if (!was_active) {
        sys_state.emergency_trigger_count++;
    }

    nvs_set_u32(sys_state.nvs_handle, NVS_KEY_EMERGENCY_STOP, 1);
    persist_runtime_counters_locked();
    xSemaphoreGive(sys_state_mutex);
}

static void reset_emergency_stop(void)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    sys_state.emergency_stop_active = false;
    strcpy(sys_state.emergency_stop_reason, "");
    nvs_set_u32(sys_state.nvs_handle, NVS_KEY_EMERGENCY_STOP, 0);
    nvs_commit(sys_state.nvs_handle);
    xSemaphoreGive(sys_state_mutex);
}

static void finalize_active_valve_session(uint64_t now_ms)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    if (sys_state.current_valve_open_start_ms == 0) {
        xSemaphoreGive(sys_state_mutex);
        return;
    }

    uint64_t open_time_ms = now_ms - sys_state.current_valve_open_start_ms;
    sys_state.total_open_time_ms += (uint32_t)open_time_ms;
    sys_state.total_liters += ((float)open_time_ms / 60000.0f) * sys_state.flow_rate_l_per_min;
    sys_state.current_valve_open_start_ms = 0;
    persist_runtime_counters_locked();
    xSemaphoreGive(sys_state_mutex);
}

/**
 * @brief Handler: GET /api/status - Return system status as JSON
 */
static esp_err_t status_handler(httpd_req_t *req)
{
    char json_response[2600];
    char escaped_emergency_reason[(sizeof(sys_state.emergency_stop_reason) * 2) + 1] = {0};
    char escaped_stack_warning_message[(sizeof(sys_state.stack_warning_message) * 2) + 1] = {0};
    char escaped_cpu_top_task[64] = {0};
    int free_mem = esp_get_free_heap_size();
    system_state_t state_snapshot;
    wifi_state_t wifi_snapshot;
    long long now_seconds = (long long)(esp_timer_get_time() / 1000000);
    long long uptime_ms = (long long)(esp_timer_get_time() / 1000);
    uint32_t cpu_core0_percent = 0;
    uint32_t cpu_core1_percent = 0;
    uint32_t cpu_top_task_percent = 0;
    uint32_t cpu_task_count = 0;
    char cpu_top_task[48] = {0};

    get_system_state_snapshot(&state_snapshot);
    get_wifi_state_snapshot(&wifi_snapshot);

    int fill_percent = calculate_fill_percent(
        state_snapshot.sensor_distance_cm,
        state_snapshot.threshold_top,
        state_snapshot.threshold_bottom
    );

    json_escape_string(state_snapshot.emergency_stop_reason,
        escaped_emergency_reason, sizeof(escaped_emergency_reason));
    json_escape_string(state_snapshot.stack_warning_message,
        escaped_stack_warning_message, sizeof(escaped_stack_warning_message));

    collect_cpu_runtime_stats(
        &cpu_core0_percent,
        &cpu_core1_percent,
        cpu_top_task,
        sizeof(cpu_top_task),
        &cpu_top_task_percent,
        &cpu_task_count
    );
    json_escape_string(cpu_top_task, escaped_cpu_top_task, sizeof(escaped_cpu_top_task));

    int json_len = snprintf(json_response, sizeof(json_response),
        "{"
        "\"status\":\"%s\","
        "\"emergency\":%s,"
        "\"emergency_reason\":\"%s\","
        "\"timestamp\":%lld,"
        "\"sensors\":{"
        "\"tank_level_cm\":%d,"
        "\"fill_percent\":%d,"
        "\"tank_full\":%d,"
        "\"last_raw_mm\":%u,"
        "\"invalid_read_count\":%lu,"
        "\"fallback_reuse_count\":%lu,"
        "\"stale\":%s"
        "},"
        "\"config\":{"
        "\"threshold_top_cm\":%lu,"
        "\"threshold_bottom_cm\":%lu,"
        "\"timeout_max_ms\":%lu,"
        "\"fill_progress_timeout_ms\":%lu,"
        "\"flow_rate_l_per_min\":%.2f"
        "},"
        "\"valve\":{"
        "\"state\":\"%s\","
        "\"manual_fill_active\":%s,"
        "\"open_count\":%lu,"
        "\"trigger_count\":%lu,"
        "\"emergency_trigger_count\":%lu,"
        "\"total_open_time_ms\":%llu,"
        "\"total_liters\":%.2f"
        "},"
        "\"system\":{"
        "\"free_heap_bytes\":%d,"
        "\"uptime_ms\":%lld,"
        "\"wifi_connected\":%s,"
        "\"stack_warning\":%s,"
        "\"stack_warning_message\":\"%s\","
        "\"cpu_core0_percent\":%lu,"
        "\"cpu_core1_percent\":%lu,"
        "\"cpu_top_task\":\"%s\","
        "\"cpu_top_task_percent\":%lu,"
        "\"cpu_task_count\":%lu,"
        "\"app_version\":\"%s\","
        "\"api_version\":\"%s\""
        "}"
        "}",
        state_snapshot.emergency_stop_active ? "EMERGENCY_STOP" : "OK",
        state_snapshot.emergency_stop_active ? "true" : "false",
        escaped_emergency_reason,
        now_seconds,
        state_snapshot.sensor_distance_cm,
        fill_percent,
        state_snapshot.sensor_distance_cm <= state_snapshot.threshold_top ? 1 : 0,
        (unsigned int)state_snapshot.sensor_last_raw_mm,
        (unsigned long)state_snapshot.sensor_invalid_read_count,
        (unsigned long)state_snapshot.sensor_fallback_reuse_count,
        state_snapshot.sensor_data_stale ? "true" : "false",
        (unsigned long)state_snapshot.threshold_top,
        (unsigned long)state_snapshot.threshold_bottom,
        (unsigned long)state_snapshot.timeout_max,
        (unsigned long)state_snapshot.fill_progress_timeout_ms,
        state_snapshot.flow_rate_l_per_min,
        state_snapshot.valve_state ? "OPEN" : "CLOSED",
        state_snapshot.manual_fill_active ? "true" : "false",
        (unsigned long)state_snapshot.valve_open_count,
        (unsigned long)state_snapshot.valve_open_count,
        (unsigned long)state_snapshot.emergency_trigger_count,
        (unsigned long long)state_snapshot.total_open_time_ms,
        state_snapshot.total_liters,
        free_mem,
        uptime_ms,
        wifi_snapshot.is_connected ? "true" : "false",
        state_snapshot.stack_warning_active ? "true" : "false",
        escaped_stack_warning_message,
        (unsigned long)cpu_core0_percent,
        (unsigned long)cpu_core1_percent,
        escaped_cpu_top_task,
        (unsigned long)cpu_top_task_percent,
        (unsigned long)cpu_task_count,
        APP_FULL_VERSION,
        API_VERSION
    );

    if (json_len < 0 || json_len >= (int)sizeof(json_response)) {
        ESP_LOGW(TAG, "status_handler JSON truncated (len=%d, cap=%d)", json_len, (int)sizeof(json_response));
    }
    
    send_json_response(req, json_response);
    return ESP_OK;
}

/**
 * @brief Handler: GET /api/config - Return configuration
 */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    char json_response[512];
    system_state_t state_snapshot;
    
    get_system_state_snapshot(&state_snapshot);

    snprintf(json_response, sizeof(json_response),
        "{"
        "\"config\":{"
        "  \"threshold_top_cm\":%u,"
        "  \"threshold_bottom_cm\":%u,"
        "  \"timeout_max_ms\":%u,"
        "  \"fill_progress_timeout_ms\":%u,"
        "  \"flow_rate_l_per_min\":%.2f"
        "}"
        "}",
        (unsigned int)state_snapshot.threshold_top,
        (unsigned int)state_snapshot.threshold_bottom,
        (unsigned int)state_snapshot.timeout_max,
        (unsigned int)state_snapshot.fill_progress_timeout_ms,
        state_snapshot.flow_rate_l_per_min
    );
    
    send_json_response(req, json_response);
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/config - Update configuration
 */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    esp_err_t recv_err = receive_request_body(req, buf, sizeof(buf));

    if (recv_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    
    // Simple JSON parsing for thresholds
    int top = -1, bottom = -1, timeout = -1, fill_progress_timeout = -1;
    float flow_rate = -1.0f;
    bool has_top = parse_json_int_field(buf, "\"threshold_top_cm\"", &top);
    bool has_bottom = parse_json_int_field(buf, "\"threshold_bottom_cm\"", &bottom);
    bool has_timeout = parse_json_int_field(buf, "\"timeout_max_ms\"", &timeout);
    bool has_fill_progress_timeout = parse_json_int_field(buf, "\"fill_progress_timeout_ms\"", &fill_progress_timeout);
    bool has_flow_rate = parse_json_float_field(buf, "\"flow_rate_l_per_min\"", &flow_rate);
    
    if (has_top && has_bottom && has_timeout && has_fill_progress_timeout && has_flow_rate &&
        top >= 0 && bottom >= 0 && timeout >= 0 && fill_progress_timeout >= 0 && flow_rate >= 0.0f) {

        if (top > 0 && bottom > top && timeout >= 1000 && fill_progress_timeout >= 1000 && flow_rate > 0.0f) {
            esp_err_t nvs_err = ESP_OK;
            nvs_err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_THRESHOLD_TOP, top);
            if (nvs_err == ESP_OK) nvs_err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_THRESHOLD_BOTTOM, bottom);
            if (nvs_err == ESP_OK) nvs_err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_VALVE_TIMEOUT_MAX, timeout);
            if (nvs_err == ESP_OK) nvs_err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_FILL_PROGRESS_TIMEOUT, fill_progress_timeout);
            if (nvs_err == ESP_OK) nvs_err = nvs_set_u32(sys_state.nvs_handle, NVS_KEY_FLOW_RATE, (uint32_t)(flow_rate * 100.0f));
            if (nvs_err == ESP_OK) nvs_err = nvs_commit(sys_state.nvs_handle);

            if (nvs_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to persist config to NVS: %s", esp_err_to_name(nvs_err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config to NVS");
                return ESP_FAIL;
            }

            update_runtime_config(top, bottom, timeout, fill_progress_timeout, flow_rate);
            
            char response[256];
            snprintf(response, sizeof(response),
                "{\"status\":\"OK\",\"message\":\"Config updated\"}");
            send_json_response(req, response);
            return ESP_OK;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON or values");
    return ESP_FAIL;
}

/**
 * @brief Handler: POST /api/valve/manual - Manual valve control
 */
static esp_err_t valve_manual_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    esp_err_t recv_err = receive_request_body(req, buf, sizeof(buf));

    if (recv_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    
    int action = -1;
    if (strstr(buf, "\"action\":\"open\"")) {
        action = 1;
    } else if (strstr(buf, "\"action\":\"close\"")) {
        action = 0;
    }

    if (action >= 0) {
        bool manual_fill_active = false;
        const char *message = NULL;
        esp_err_t request_err = request_manual_fill(action == 1, "API", &manual_fill_active, &message);

        if (request_err == ESP_ERR_INVALID_STATE) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message ? message : "Emergency stop active");
            return ESP_FAIL;
        }
        
        char response[256];
        snprintf(response, sizeof(response),
            "{\"status\":\"OK\",\"action\":\"%s\",\"manual_fill_active\":%s,\"message\":\"%s\"}",
            manual_fill_active ? "open" : "close",
            manual_fill_active ? "true" : "false",
            message ? message : "OK");
        send_json_response(req, response);
        return ESP_OK;
    }
    
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action");
    return ESP_FAIL;
}

/**
 * @brief Handler: POST /api/emergency_stop - Emergency stop
 * Closes valve and disables auto-filling until reset
 */
static esp_err_t emergency_stop_handler(httpd_req_t *req)
{
    char buf[64] = {0};
    esp_err_t recv_err = receive_request_body(req, buf, sizeof(buf));
    system_state_t state_snapshot;
    bool do_reset = true;

    if (recv_err == ESP_OK && strstr(buf, "\"action\":\"trigger\"")) {
        do_reset = false;
    }

    get_system_state_snapshot(&state_snapshot);

    if (do_reset) {
        if (state_snapshot.emergency_stop_active) {
            ESP_LOGI(TAG, "♻️  EMERGENCY STOP RESET - system resuming normal operations");
            reset_emergency_stop();
        } else {
            ESP_LOGI(TAG, "Reset requested while no emergency stop was active");
        }
    } else {
        ESP_LOGW(TAG, "🚨 EMERGENCY STOP TRIGGERED - all operations halted!");
        trigger_emergency_stop("Emergency activated via API");
    }
    
    // Always close valve when emergency button is pressed
    gpio_set_level(GPIO_VALVE_CONTROL, 0);
    set_valve_and_manual_state(false, false);
    finalize_active_valve_session(esp_timer_get_time() / 1000);
    get_system_state_snapshot(&state_snapshot);
    
    char response[256];
    snprintf(response, sizeof(response),
        "{\"status\":\"%s\",\"emergency\":%s,\"message\":\"%s\",\"valve\":\"CLOSED\"}",
        state_snapshot.emergency_stop_active ? "EMERGENCY" : "OK",
        state_snapshot.emergency_stop_active ? "true" : "false",
        state_snapshot.emergency_stop_active ? "Emergency activated" : "Emergency reset"
    );
    send_json_response(req, response);
    
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/valve/stop - Force valve closed
 */
static esp_err_t valve_stop_handler(httpd_req_t *req)
{
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    sys_state.user_fill_halt = true;
    xSemaphoreGive(sys_state_mutex);
    gpio_set_level(GPIO_VALVE_CONTROL, 0);
    set_valve_and_manual_state(false, false);
    finalize_active_valve_session(esp_timer_get_time() / 1000);
    
    char response[128];
    snprintf(response, sizeof(response),
        "{\"status\":\"OK\",\"valve\":\"CLOSED\",\"message\":\"Valve stopped\"}"
    );
    send_json_response(req, response);
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/counters/reset - Reset persistent runtime counters
 */
static esp_err_t counters_reset_handler(httpd_req_t *req)
{
    system_state_t state_snapshot;
    get_system_state_snapshot(&state_snapshot);

    if (state_snapshot.valve_state || state_snapshot.manual_fill_active) {
        send_json_response(req,
            "{\"status\":\"ERROR\",\"message\":\"Ventil erst schliessen\"}");
        return ESP_OK;
    }

    reset_runtime_counters();
    send_json_response(req,
        "{\"status\":\"OK\",\"message\":\"Zaehler zurueckgesetzt\"}");
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/warnings/reset - Clear sticky runtime warnings
 */
static esp_err_t warnings_reset_handler(httpd_req_t *req)
{
    bool cleared = clear_runtime_warnings();

    send_json_response(req,
        cleared
            ? "{\"status\":\"OK\",\"message\":\"Warnmeldungen zurueckgesetzt\"}"
            : "{\"status\":\"OK\",\"message\":\"Keine Warnmeldungen aktiv\"}");
    return ESP_OK;
}

/**
 * @brief Handler: GET /api/ota/status - OTA runtime status
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    ota_state_t snapshot;
    char escaped_phase[(sizeof(snapshot.phase) * 2) + 1] = {0};
    char escaped_message[(sizeof(snapshot.message) * 2) + 1] = {0};
    char escaped_error[(sizeof(snapshot.last_error) * 2) + 1] = {0};
    char escaped_current_version[(sizeof(snapshot.current_version) * 2) + 1] = {0};
    char escaped_target_version[(sizeof(snapshot.target_version) * 2) + 1] = {0};
    char response[768];

    get_ota_state_snapshot(&snapshot);
    json_escape_string(snapshot.phase, escaped_phase, sizeof(escaped_phase));
    json_escape_string(snapshot.message, escaped_message, sizeof(escaped_message));
    json_escape_string(snapshot.last_error, escaped_error, sizeof(escaped_error));
    json_escape_string(snapshot.current_version, escaped_current_version, sizeof(escaped_current_version));
    json_escape_string(snapshot.target_version, escaped_target_version, sizeof(escaped_target_version));

    snprintf(response, sizeof(response),
        "{\"status\":\"%s\",\"ota\":{"
        "\"in_progress\":%s,"
        "\"last_result_ok\":%s,"
        "\"phase\":\"%s\","
        "\"message\":\"%s\","
        "\"last_error\":\"%s\","
        "\"current_version\":\"%s\","
        "\"target_version\":\"%s\","
        "\"last_start_ms\":%llu,"
        "\"last_end_ms\":%llu"
        "}}",
        snapshot.in_progress ? "BUSY" : "OK",
        snapshot.in_progress ? "true" : "false",
        snapshot.last_result_ok ? "true" : "false",
        escaped_phase,
        escaped_message,
        escaped_error,
        escaped_current_version,
        escaped_target_version,
        (unsigned long long)snapshot.last_start_ms,
        (unsigned long long)snapshot.last_end_ms);

    send_json_response(req, response);
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/ota/start - Start OTA from HTTP(S) URL
 */
static esp_err_t ota_start_handler(httpd_req_t *req)
{
    char body[320] = {0};
    char url[192] = {0};
    ota_state_t snapshot;

    esp_err_t recv_err = receive_request_body(req, body, sizeof(body));
    if (recv_err != ESP_OK) {
        send_json_response(req,
            "{\"status\":\"ERROR\",\"message\":\"Ungueltiger Request\"}");
        return ESP_OK;
    }

    if (!parse_json_string_field(body, "\"url\"", url, sizeof(url)) || strlen(url) < 12) {
        send_json_response(req,
            "{\"status\":\"ERROR\",\"message\":\"URL fehlt\"}");
        return ESP_OK;
    }

    bool is_https = (strncmp(url, "https://", 8) == 0);
    bool is_http = (strncmp(url, "http://", 7) == 0);
    if (!is_https && !is_http) {
        send_json_response(req,
            "{\"status\":\"ERROR\",\"message\":\"URL muss mit http:// oder https:// beginnen\"}");
        return ESP_OK;
    }

    get_ota_state_snapshot(&snapshot);
    if (snapshot.in_progress || ota_task_handle != NULL) {
        send_json_response(req,
            "{\"status\":\"ERROR\",\"message\":\"OTA bereits aktiv\"}");
        return ESP_OK;
    }

    char *task_url = strdup(url);
    if (task_url == NULL) {
        send_json_response(req,
            "{\"status\":\"ERROR\",\"message\":\"Kein Speicher fuer OTA\"}");
        return ESP_OK;
    }

    xSemaphoreTake(ota_state_mutex, portMAX_DELAY);
    ota_state.in_progress = true;
    ota_state.last_result_ok = false;
    strncpy(ota_state.phase, "STARTING", sizeof(ota_state.phase) - 1);
    strncpy(ota_state.message, "OTA wird gestartet", sizeof(ota_state.message) - 1);
    ota_state.last_error[0] = '\0';
    strncpy(ota_state.current_version, APP_FULL_VERSION, sizeof(ota_state.current_version) - 1);
    ota_state.target_version[0] = '\0';
    strncpy(ota_state.url, url, sizeof(ota_state.url) - 1);
    ota_state.last_start_ms = (uint64_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(ota_state_mutex);

    BaseType_t created = xTaskCreatePinnedToCore(
        ota_update_task,
        "ota_task",
        TASK_STACK_SERVER,
        task_url,
        TASK_PRIO_MAIN,
        &ota_task_handle,
        TASK_CORE_NETWORK);

    if (created != pdPASS) {
        free(task_url);
        xSemaphoreTake(ota_state_mutex, portMAX_DELAY);
        ota_state.in_progress = false;
        strncpy(ota_state.phase, "FAILED", sizeof(ota_state.phase) - 1);
        strncpy(ota_state.message, "OTA Task konnte nicht gestartet werden", sizeof(ota_state.message) - 1);
        strncpy(ota_state.last_error, "task-create-failed", sizeof(ota_state.last_error) - 1);
        ota_state.last_end_ms = (uint64_t)(esp_timer_get_time() / 1000);
        xSemaphoreGive(ota_state_mutex);

        send_json_response(req,
            "{\"status\":\"ERROR\",\"message\":\"OTA Task konnte nicht gestartet werden\"}");
        return ESP_OK;
    }

    send_json_response(req,
        "{\"status\":\"OK\",\"message\":\"OTA gestartet\"}");
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/sensor/reset - Request TOF sensor reinitialization
 */
static esp_err_t sensor_reset_handler(httpd_req_t *req)
{
    sensor_reinit_requested = true;
    send_json_response(req,
        "{\"status\":\"OK\",\"message\":\"Sensor-Reinit angefordert\"}");
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/system/reset - Soft reset (keep WiFi)
 */
static esp_err_t system_reset_handler(httpd_req_t *req)
{
    gpio_set_level(GPIO_VALVE_CONTROL, 0);
    set_valve_and_manual_state(false, false);
    finalize_active_valve_session(esp_timer_get_time() / 1000);
    persist_runtime_counters();

    // Just send response, then restart (keeps WiFi credentials in NVS)
    char response[100];
    snprintf(response, sizeof(response),
        "{\"status\":\"OK\",\"message\":\"System restarting...\"}"
    );
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    
    // Restart after response sent
    esp_restart();
    return ESP_OK;
}

static void ota_update_task(void *pvParameters)
{
    char *url = (char *)pvParameters;
    esp_err_t err = ESP_FAIL;
    esp_https_ota_handle_t ota_handle = NULL;
    bool ota_begun = false;
    bool ota_aborted = false;

    xSemaphoreTake(ota_state_mutex, portMAX_DELAY);
    strncpy(ota_state.phase, "DOWNLOADING", sizeof(ota_state.phase) - 1);
    strncpy(ota_state.message, "Firmware wird geladen", sizeof(ota_state.message) - 1);
    xSemaphoreGive(ota_state_mutex);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err == ESP_OK) {
        ota_begun = true;
        esp_app_desc_t app_desc;
        if (esp_https_ota_get_img_desc(ota_handle, &app_desc) == ESP_OK) {
            xSemaphoreTake(ota_state_mutex, portMAX_DELAY);
            strncpy(ota_state.target_version, app_desc.version, sizeof(ota_state.target_version) - 1);
            xSemaphoreGive(ota_state_mutex);
        }

        while ((err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota_handle)) {
            err = esp_https_ota_finish(ota_handle);
        } else {
            if (err == ESP_OK) {
                err = ESP_FAIL;
            }
            esp_https_ota_abort(ota_handle);
            ota_aborted = true;
        }
    }

    if (err == ESP_OK) {
        xSemaphoreTake(ota_state_mutex, portMAX_DELAY);
        ota_state.in_progress = false;
        ota_state.last_result_ok = true;
        strncpy(ota_state.phase, "SUCCESS", sizeof(ota_state.phase) - 1);
        strncpy(ota_state.message, "OTA erfolgreich, Neustart folgt", sizeof(ota_state.message) - 1);
        ota_state.last_error[0] = '\0';
        ota_state.last_end_ms = (uint64_t)(esp_timer_get_time() / 1000);
        xSemaphoreGive(ota_state_mutex);

        ESP_LOGI(TAG, "✅ OTA successful, restarting device");
        ota_task_handle = NULL;
        free(url);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    if (ota_begun && err != ESP_OK && !ota_aborted) {
        // Ensure handle is not leaked in case finish path was not reached.
        esp_https_ota_abort(ota_handle);
    }

    xSemaphoreTake(ota_state_mutex, portMAX_DELAY);
    ota_state.in_progress = false;
    ota_state.last_result_ok = false;
    strncpy(ota_state.phase, "FAILED", sizeof(ota_state.phase) - 1);
    strncpy(ota_state.message, "OTA fehlgeschlagen", sizeof(ota_state.message) - 1);
    strncpy(ota_state.last_error, esp_err_to_name(err), sizeof(ota_state.last_error) - 1);
    ota_state.last_end_ms = (uint64_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(ota_state_mutex);

    ota_task_handle = NULL;
    free(url);
    vTaskDelete(NULL);
}

/**
 * @brief Handler: GET /api/wifi/status - WiFi connection status
 */
static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    // Get current WiFi mode and status
    wifi_ap_record_t ap_info;
    char response[512];
    char connected_ssid[sizeof(ap_info.ssid) + 1] = {0};
    char escaped_ssid[(sizeof(ap_info.ssid) * 2) + 1] = {0};
    wifi_state_t wifi_snapshot;
    
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    get_wifi_state_snapshot(&wifi_snapshot);
    
    // Get local IP if connected
    char ip_str[16] = "Not connected";
    if (err == ESP_OK) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(netif, &ip_info);
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }

        snprintf(connected_ssid, sizeof(connected_ssid), "%.*s",
            (int)sizeof(ap_info.ssid), (const char *)ap_info.ssid);
    }

    const char *ssid_source = (err == ESP_OK) ? connected_ssid : wifi_snapshot.ssid;
    if (ssid_source[0] == '\0') {
        ssid_source = "Not connected";
    }
    json_escape_string(ssid_source, escaped_ssid, sizeof(escaped_ssid));
    
    snprintf(response, sizeof(response),
        "{\"wifi\":"
        "{\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"connected\":%s,"
        "\"ip\":\"%s\"}}", 
        escaped_ssid,
        (err == ESP_OK) ? ap_info.rssi : 0,
        wifi_snapshot.is_connected ? "true" : "false",
        ip_str
    );
    send_json_response(req, response);
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/wifi/config - Update WiFi credentials and attempt connection
 */
static esp_err_t wifi_config_handler(httpd_req_t *req)
{
    // Read content length
    size_t content_len = req->content_len;
    if (content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too large");
        return ESP_FAIL;
    }
    
    // Read POST body
    char buffer[1024] = {0};
    esp_err_t recv_err = receive_request_body(req, buffer, sizeof(buffer));
    if (recv_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    
    // Extract SSID and Password using simple string parsing
    // Format: {"ssid":"xxxx","password":"yyyy"}
    char ssid[32] = {0};
    char password[64] = {0};
    bool has_ssid = parse_json_string_field(buffer, "\"ssid\"", ssid, sizeof(ssid));
    bool has_password = parse_json_string_field(buffer, "\"password\"", password, sizeof(password));
    
    // Validate input
    if (!has_ssid || !has_password || strlen(ssid) == 0 || strlen(password) == 0) {
        char response[100];
        snprintf(response, sizeof(response),
            "{\"status\":\"ERROR\",\"message\":\"SSID and password required\"}"
        );
        send_json_response(req, response);
        return ESP_OK;
    }
    
    // Save to NVS
    esp_err_t ret_ssid = nvs_set_str(sys_state.nvs_handle, NVS_KEY_WIFI_SSID, ssid);
    esp_err_t ret_pass = nvs_set_str(sys_state.nvs_handle, NVS_KEY_WIFI_PASS, password);
    
    if (ret_ssid != ESP_OK || ret_pass != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi credentials to NVS");
        char response[100];
        snprintf(response, sizeof(response),
            "{\"status\":\"ERROR\",\"message\":\"Failed to save credentials\"}"
        );
        send_json_response(req, response);
        return ESP_OK;
    }
    
    // Commit changes
    esp_err_t ret_commit = nvs_commit(sys_state.nvs_handle);
    if (ret_commit != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit WiFi credentials: %s", esp_err_to_name(ret_commit));
        char response[100];
        snprintf(response, sizeof(response),
            "{\"status\":\"ERROR\",\"message\":\"Failed to commit credentials\"}"
        );
        send_json_response(req, response);
        return ESP_OK;
    }
    
    // Update in-memory state
    update_wifi_credentials_state(ssid);
    
    // Apply new WiFi credentials immediately
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    
    // If was in AP-only fallback, ensure APSTA mode for reconnect
    wifi_state_t wifi_snapshot;
    get_wifi_state_snapshot(&wifi_snapshot);
    if (wifi_snapshot.ap_active) {
        ESP_LOGI(TAG, "Leaving fallback AP, attempting STA reconnect");
        set_fallback_ap_enabled(false);
    }
    
    // Disconnect any existing STA connection, then reconnect
    esp_wifi_disconnect();
    esp_wifi_connect();
    
    ESP_LOGI(TAG, "WiFi: Connecting to '%s'", ssid);
    
    char escaped_ssid[(sizeof(ssid) * 2) + 1] = {0};
    char response[240];
    json_escape_string(ssid, escaped_ssid, sizeof(escaped_ssid));
    snprintf(response, sizeof(response),
        "{\"status\":\"OK\",\"message\":\"WiFi credentials saved. Connecting to '%s'...\"}",
        escaped_ssid
    );
    send_json_response(req, response);
    return ESP_OK;
}

/**
 * @brief Captive portal: redirect to root page (302)
 */
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_MODE_IP_ADDR "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief Minimal DNS server task for captive portal
 * Responds to ALL DNS queries with our AP IP (10.1.1.1)
 * This makes phones/laptops detect the captive portal automatically
 */
static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buf[512];
    uint8_t tx_buf[512];

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: Failed to create socket");
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval recv_timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: Failed to bind port 53");
        close(sock);
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS captive portal server started on port 53");

    struct sockaddr_in client_addr;
    
    // Our AP IP in network byte order
    uint32_t ap_ip = esp_ip4addr_aton(AP_MODE_IP_ADDR);
    
    while (!dns_server_stop_requested) {
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&client_addr, &addr_len);
        if (len < 0) {
            continue;
        }
        if (len < 12) continue;  // DNS header is 12 bytes minimum
        if ((len + 16) > 512) continue;
        
        // Build DNS response: copy query, set response flags, append answer
        memcpy(tx_buf, rx_buf, len);
        
        // Set response flags: QR=1 (response), AA=1 (authoritative), RD=1, RA=1
        tx_buf[2] = 0x84;  // QR=1, Opcode=0, AA=1
        tx_buf[3] = 0x00;  // RCODE=0 (no error)
        
        // Set answer count = 1
        tx_buf[6] = 0x00;
        tx_buf[7] = 0x01;
        
        // Append answer section after the query
        int pos = len;
        // Name pointer to question (0xC00C = offset 12)
        tx_buf[pos++] = 0xC0;
        tx_buf[pos++] = 0x0C;
        // Type A (1)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x01;
        // Class IN (1)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x01;
        // TTL = 60 seconds
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x3C;
        // RDLENGTH = 4 (IPv4)
        tx_buf[pos++] = 0x00;
        tx_buf[pos++] = 0x04;
        // RDATA = our IP
        memcpy(&tx_buf[pos], &ap_ip, 4);
        pos += 4;
        
        sendto(sock, tx_buf, pos, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }

    close(sock);
    dns_server_stop_requested = false;
    dns_task_handle = NULL;
    ESP_LOGI(TAG, "DNS captive portal server stopped");
    vTaskDelete(NULL);
}

/**
 * @brief Handler: GET / - Root HTML page with modern UI
 */
static esp_err_t index_handler(httpd_req_t *req)
{
    // Lightweight HTML UI - no emojis, minimal size for reliable transfer
    static const char index_html[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BOSCH TANK</title>
<style>
body{font-family:Arial,sans-serif;background:linear-gradient(180deg,#5d76df 0%,#7ea7ff 100%);margin:0;padding:10px}
.container{max-width:460px;margin:0 auto;background:white;padding:12px;border-radius:14px;box-shadow:0 8px 24px rgba(15,23,42,0.18)}
h1{color:#1f2937;margin:0;font-size:19px}
.tabs{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:5px;margin:12px 0;border-bottom:2px solid #e5e7eb}
.tab-btn{min-width:0;padding:9px 6px;border:none;background:#eef2f7;cursor:pointer;font-weight:bold;border-radius:8px 8px 0 0;font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tab-btn.active{background:#3056d3;color:white}
.tab-content{display:none;padding:10px 0 0 0}
.tab-content.active{display:block}
.dashboard-hero{display:grid;grid-template-columns:minmax(150px,1.1fr) minmax(0,1fr);gap:10px;margin:8px 0 10px 0}
.tank-panel{background:linear-gradient(180deg,#ecf5ff 0%,#dbeafe 100%);border-radius:12px;padding:12px;text-align:center;border:1px solid #bfdbfe}
.tank-sub{font-size:12px;color:#5b6472}
.big-num{font-size:38px;font-weight:bold;color:#0f4ab8;line-height:1}
.percent{font-size:17px;color:#0f4ab8;margin-top:6px;font-weight:bold}
.tank-bar{margin-top:10px;height:14px;background:#d6dae1;border-radius:999px;position:relative;overflow:hidden}
.summary-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.stat-card{background:#f8fbff;border:1px solid #d7e6ff;border-radius:10px;padding:9px;min-height:66px}
.stat-label{font-size:10px;color:#526071;margin-bottom:3px}
.stat-value{font-size:18px;font-weight:bold;color:#174ea6;line-height:1.1}
.stat-sub{font-size:10px;color:#6b7280;margin-top:2px}
.status-row{display:flex;justify-content:space-between;padding:8px;background:#f0f0f0;margin:5px 0;border-radius:4px;font-size:12px}
.status-pills{display:grid;grid-template-columns:1fr 1fr;gap:7px;margin:0 0 10px 0}
.pill{background:#f3f4f6;border-radius:999px;padding:8px 10px;font-size:11px;display:flex;justify-content:space-between;gap:8px}
.pill b{color:#111827}
.pill-valve-closed{background:#e5e7eb}
.pill-valve-open{background:#d1d5db;animation:valveBlink 1s steps(1,end) infinite}
.pill-valve-open b{color:#7f1d1d}
@keyframes valveBlink{0%,49%{background:#ef4444}50%,100%{background:#d1d5db}}
.buttons{display:flex;gap:8px;margin:10px 0 8px 0;flex-wrap:wrap}
button{flex:1;min-width:90px;padding:10px;border:none;border-radius:8px;font-weight:bold;cursor:pointer;font-size:12px}
.btn-primary{background:#667eea;color:white}
.btn-danger{background:#f44336;color:white}
.btn-success{background:#4caf50;color:white}
.btn-secondary{background:#ddd}
.btn-reset{background:#4caf50;color:white}
.btn-reset.active{background:#f44336;color:white}
.btn-reset.holding{background:#ff9800;color:white}
.status-ok{color:#2e7d32;font-weight:bold}
.status-emergency{color:#c62828;font-weight:bold}
.persistent-warning{display:none;padding:10px 12px;background:#fff3cd;border:1px solid #f59e0b;border-radius:8px;margin:8px 0 0 0;font-size:12px;color:#92400e;font-weight:bold}
.diag-box{margin:8px 0 0 0;padding:8px 10px;border:1px solid #d1d5db;border-radius:8px;background:#f9fafb;font-size:11px;color:#374151;line-height:1.4}
.diag-stale{color:#b91c1c;font-weight:bold}
.diag-live{color:#166534;font-weight:bold}
label{display:block;font-weight:bold;margin:8px 0 4px 0;font-size:12px;color:#333}
input{width:100%;padding:8px;margin:0 0 8px 0;box-sizing:border-box;border-radius:4px;border:1px solid #ddd;font-size:14px}
.msg{padding:10px;margin:8px 0;border-radius:4px;font-size:12px}
.error{background:#ffebee;color:#f44336}
.success{background:#e8f5e9;color:#4caf50}
@media (max-width:390px){.dashboard-hero{grid-template-columns:1fr}.container{padding:10px}.big-num{font-size:34px}.tabs{grid-template-columns:repeat(2,minmax(0,1fr));border-bottom:none}.tab-btn{border-radius:8px;font-size:11px;padding:8px 5px}}
</style></head>
<body>
<div class="container">
<h1>BOSCH TANK</h1>

<div class="tabs">
<button class="tab-btn active" onclick="switchTab(event, 'dashboard')">Dashboard</button>
<button class="tab-btn" onclick="switchTab(event, 'settings')">Settings</button>
<button class="tab-btn" onclick="switchTab(event, 'wifi')">WiFi</button>
<button class="tab-btn" onclick="switchTab(event, 'diagnostics')">Diagnose</button>
</div>

<!-- DASHBOARD TAB -->
<div class="tab-content active" id="dashboard">
<div class="dashboard-hero">
<div class="tank-panel">
<div class="big-num" id="level">--</div>
<div class="tank-sub">cm Wasserstand</div>
<div id="percent" class="percent">--</div>
<div class="tank-bar">
<div id="bar-fill" style="height:100%;background:#4caf50;width:0%"></div>
</div>
</div>
<div class="summary-grid">
<div class="stat-card"><div class="stat-label">Literzaehler</div><div class="stat-value" id="total-liters">--</div><div class="stat-sub">Gesamtmenge</div></div>
<div class="stat-card"><div class="stat-label">Ventil-Ausloesungen</div><div class="stat-value" id="open-count">--</div><div class="stat-sub">Auto + manuell</div></div>
<div class="stat-card"><div class="stat-label">Notaus-Ausloesungen</div><div class="stat-value" id="emergency-count">--</div><div class="stat-sub">Persistenter Zaehler</div></div>
<div class="stat-card"><div class="stat-label">Oeffnungszeit</div><div class="stat-value" id="total-time">--</div><div class="stat-sub">Gesamt in Sekunden</div></div>
</div>
</div>
<div class="status-pills">
<div class="pill"><span>System</span><b id="status">OK</b></div>
<div class="pill pill-valve-closed" id="valve-pill"><span>Ventil</span><b id="valve">GESCHL</b></div>
<div class="pill"><span>WiFi</span><b id="dash-wifi">OK</b></div>
<div class="pill"><span>Notaus</span><b id="emergency-state" class="status-ok">FREI</b></div>
</div>
<div id="emergency-reason" style="display:none;padding:8px;background:#ffebee;border-radius:4px;margin:5px 0;font-size:12px;color:#c62828"></div>
<div id="stack-warning" class="persistent-warning"></div>

<div class="buttons">
<button class="btn-primary" id="fill-btn" onclick="fill()">BEFUELLEN</button>
<button class="btn-danger" onclick="stop()">STOPP</button>
<button class="btn-reset" id="emergency-btn" onclick="resetEmergency()">ZAEHLER 3S</button>
</div>
<div style="margin-top:10px;text-align:center;font-size:11px;color:#777">App <span id="app-version">-</span></div>
<div id="msg-dashboard" class="msg" style="display:none"></div>
</div>

<!-- SETTINGS TAB -->
<div class="tab-content" id="settings">
<p style="font-size:12px;margin:0 0 10px 0"><b>Schwellenwerte:</b></p>
<label for="top">OBEN (cm):</label>
<input type="number" id="top" min="1" max="100">
<label for="bottom">UNTEN (cm):</label>
<input type="number" id="bottom" min="1" max="100">
<label for="timeout">Timeout (ms):</label>
<input type="number" id="timeout" min="1000" max="999999">
<label for="fill-progress-timeout">Fuellfortschritt Timeout (ms):</label>
<input type="number" id="fill-progress-timeout" min="1000" max="60000">
<label for="flow-rate">Durchfluss (L/min):</label>
<input type="number" id="flow-rate" step="0.1" min="0.1" max="50">
<div class="buttons">
<button class="btn-success" id="save-btn" onclick="saveSettings()">Speichern</button>
</div>
<div id="msg-settings" class="msg" style="display:none"></div>
</div>

<!-- WiFi TAB -->
<div class="tab-content" id="wifi">
<div class="status-row"><span>Status:</span><span id="wifi-con">Lädt...</span></div>
<div class="status-row"><span>SSID:</span><span id="wifi-ssid">-</span></div>
<div class="status-row"><span>Signal:</span><span id="wifi-rssi">-</span></div>
<div class="status-row"><span>IP:</span><span id="wifi-ip">-</span></div>

<div style="margin-top:20px;padding-top:15px;border-top:1px solid #ddd">
<p style="font-size:12px;margin:0 0 10px 0"><b>Neue Verbindung:</b></p>
<label for="new-ssid">SSID:</label>
<input type="text" id="new-ssid" placeholder="z.B. MeinWiFi">
<label for="new-pass">Passwort:</label>
<input type="password" id="new-pass" placeholder="Min. 8 Zeichen">
<div class="buttons">
<button class="btn-success" onclick="connectWiFi()">Verbinden</button>
<button class="btn-secondary" onclick="reset()">RESET</button>
</div>
</div>
<div id="msg-wifi" class="msg" style="display:none"></div>
</div>

<!-- DIAGNOSTICS TAB -->
<div class="tab-content" id="diagnostics">
<p style="font-size:12px;margin:0 0 10px 0"><b>Live-Diagnose:</b></p>
<div id="sensor-diag" class="diag-box">Sensor: --</div>
<div class="status-row"><span>CPU Core 0:</span><span id="cpu-core0">-</span></div>
<div class="status-row"><span>CPU Core 1:</span><span id="cpu-core1">-</span></div>
<div class="status-row"><span>Top Task:</span><span id="cpu-top-task">-</span></div>
<div class="status-row"><span>Task Last:</span><span id="cpu-top-task-pct">-</span></div>
<div class="status-row"><span>Tasks Gesamt:</span><span id="cpu-task-count">-</span></div>
<div style="margin:10px 0 4px 0;font-weight:700;font-size:13px;">OTA & Firmware</div>
<div class="status-row"><span>OTA-Status:</span><span id="ota-status">-</span></div>
<div class="status-row"><span>OTA-Phase:</span><span id="ota-phase">-</span></div>
<div class="status-row"><span>OTA-Meldung:</span><span id="ota-message">-</span></div>
<div class="status-row"><span>In Arbeit:</span><span id="ota-in-progress">-</span></div>
<div class="status-row"><span>Aktuelle Version:</span><span id="ota-current-version">-</span></div>
<div class="status-row"><span>Zielversion:</span><span id="ota-target-version">-</span></div>
<div class="status-row"><span>Letztes Ergebnis:</span><span id="ota-last-result">-</span></div>
<div class="status-row"><span>Letzter Fehler:</span><span id="ota-last-error">-</span></div>
<div class="status-row"><span>Start seit Boot:</span><span id="ota-last-start">-</span></div>
<div class="status-row"><span>Ende seit Boot:</span><span id="ota-last-end">-</span></div>
<div style="margin-top:10px;padding-top:10px;border-top:1px solid #ddd">
<label for="ota-url">Firmware-URL:</label>
<input type="text" id="ota-url" value="http://192.168.1.191/bosch-tank.bin" placeholder="http://192.168.1.191/bosch-tank.bin">
<div class="buttons"><button class="btn-success" id="ota-start-btn" onclick="startOTA()">OTA starten</button></div>
<div style="margin-top:6px;font-size:11px;color:#6b7280">Voreingestellt auf Laptop-IP 192.168.1.191.</div>
</div>
<div style="margin-top:8px;font-size:11px;color:#6b7280">Hinweis: Doppeltipp auf gruene Reset-Taste startet Sensor-Reinit.</div>
<div id="msg-diagnostics" class="msg" style="display:none"></div>
</div>

</div>

<script>
let isFilling = false;
let isEmergencyActive = false;
let isTankFull = false;
let fillActionInFlight = false;
let dashboardTimer = null;
let dashboardPollInFlight = false;
let settingsSaveInFlight = false;
let counterResetInFlight = false;
let hasStickyWarning = false;
let resetHoldTimer = null;
let resetHoldTriggered = false;
let lastResetTapMs = 0;
let lastStatusTimestamp = 0;
let stagnantStatusCount = 0;
const DASHBOARD_REFRESH_MS = 350;
const DIAGNOSTICS_REFRESH_MS = 2500;
const OTA_BINARY_NAME = 'bosch-tank.bin';
function setDefaultOtaUrl() {
    const input = document.getElementById('ota-url');
    if (!input) return;
    input.value = `http://192.168.1.191/${OTA_BINARY_NAME}`;
}
function scheduleDashboardRefresh(delay){
    if(dashboardTimer) clearTimeout(dashboardTimer);
    dashboardTimer = setTimeout(() => updateDashboard(), delay);
}
let diagnosticsPollTimer = null;
let diagnosticsPollInFlight = false;
function scheduleDiagnosticsRefresh(delay = DIAGNOSTICS_REFRESH_MS){
    if(diagnosticsPollTimer) clearTimeout(diagnosticsPollTimer);
    diagnosticsPollTimer = setTimeout(() => {
        if(document.getElementById('diagnostics')?.classList.contains('active')){
            loadDiagnostics();
        }
    }, delay);
}
function clearDiagnosticsRefresh(){
    if(diagnosticsPollTimer) clearTimeout(diagnosticsPollTimer);
    diagnosticsPollTimer = null;
}
function loadDiagnostics(force){
    if(diagnosticsPollInFlight && !force) return;
    diagnosticsPollInFlight = true;
    fetch('/api/ota/status?t=' + Date.now(), {cache: 'no-store'})
        .then(r => { if(!r.ok) throw new Error('API error: ' + r.status); return r.json(); })
        .then(d => {
            const ota = d.ota || {};
            const statusEl = document.getElementById('ota-status');
            const phaseEl = document.getElementById('ota-phase');
            const messageEl = document.getElementById('ota-message');
            const inProgressEl = document.getElementById('ota-in-progress');
            const currentVersionEl = document.getElementById('ota-current-version');
            const targetVersionEl = document.getElementById('ota-target-version');
            const lastResultEl = document.getElementById('ota-last-result');
            const lastErrorEl = document.getElementById('ota-last-error');
            const lastStartEl = document.getElementById('ota-last-start');
            const lastEndEl = document.getElementById('ota-last-end');
            const otaButton = document.getElementById('ota-start-btn');
            if(statusEl) statusEl.textContent = d.status || '-';
            if(phaseEl) phaseEl.textContent = ota.phase || '-';
            if(messageEl) messageEl.textContent = ota.message || '-';
            if(inProgressEl) inProgressEl.textContent = ota.in_progress ? 'Ja' : 'Nein';
            if(currentVersionEl) currentVersionEl.textContent = ota.current_version || '-';
            if(targetVersionEl) targetVersionEl.textContent = ota.target_version || '-';
            if(lastResultEl) lastResultEl.textContent = ota.last_result_ok ? 'OK' : 'FAIL';
            if(lastErrorEl) lastErrorEl.textContent = ota.last_error || '-';
            if(lastStartEl) lastStartEl.textContent = ota.last_start_ms ? (Number(ota.last_start_ms) / 1000).toFixed(1) + ' s' : '-';
            if(lastEndEl) lastEndEl.textContent = ota.last_end_ms ? (Number(ota.last_end_ms) / 1000).toFixed(1) + ' s' : '-';
            if(otaButton) otaButton.disabled = ota.in_progress;
        })
        .catch(e => {
            console.error('loadDiagnostics failed:', e);
            const msgEl = document.getElementById('msg-diagnostics');
            if(msgEl){
                msgEl.textContent = 'OTA-Status konnte nicht geladen werden';
                msgEl.className = 'msg error';
                msgEl.style.display = 'block';
            }
        })
        .finally(() => {
            diagnosticsPollInFlight = false;
            if(document.getElementById('diagnostics')?.classList.contains('active')){
                scheduleDiagnosticsRefresh();
            }
        });
}
function startOTA(){
    const urlInput = document.getElementById('ota-url');
    const url = urlInput ? urlInput.value.trim() : '';
    if(!url){
        showMsg('diagnostics', 'Bitte OTA-URL eingeben', true);
        return;
    }
    const btn = document.getElementById('ota-start-btn');
    if(btn){ btn.disabled = true; btn.style.opacity = '0.6'; }
    fetch('/api/ota/start', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({url})
    }).then(async r => {
        if(!r.ok) throw new Error(await r.text() || ('API error: '+r.status));
        return r.json();
    }).then(d => {
        showMsg('diagnostics', d.message || 'OTA gestartet', false);
        loadDiagnostics(true);
    }).catch(e => {
        console.error('startOTA failed:', e);
        showMsg('diagnostics', 'OTA start fehlgeschlagen: ' + e.message, true);
    }).finally(() => {
        if(btn){ btn.disabled = false; btn.style.opacity = '1'; }
    });
}
function syncSaveButton(){
    const btn = document.getElementById('save-btn');
    if(!btn) return;
    btn.disabled = settingsSaveInFlight;
    btn.style.opacity = settingsSaveInFlight ? '0.6' : '1';
    btn.textContent = settingsSaveInFlight ? 'Speichert...' : 'Speichern';
}
function syncFillButton(){
    const btn = document.getElementById('fill-btn');
    if(btn){
        if(fillActionInFlight){
            btn.textContent = '...';
        } else {
            btn.textContent = isFilling ? 'BEFUELLEN STOPPEN' : 'BEFUELLEN';
        }
        const disabled = isEmergencyActive || isTankFull || fillActionInFlight;
        btn.disabled = disabled;
        btn.style.opacity = disabled ? '0.5' : '1';
    }
}
function syncValveIndicator(open){
    const valveEl = document.getElementById('valve');
    const pillEl = document.getElementById('valve-pill');
    if(valveEl) valveEl.textContent = open ? 'OFFEN' : 'GESCHL';
    if(!pillEl) return;
    pillEl.classList.remove('pill-valve-open', 'pill-valve-closed');
    pillEl.classList.add(open ? 'pill-valve-open' : 'pill-valve-closed');
}
function syncStackWarning(message){
    const warningEl = document.getElementById('stack-warning');
    hasStickyWarning = !!message;
    if(!warningEl) return;
    if(message){
        warningEl.textContent = message;
        warningEl.style.display = 'block';
    } else {
        warningEl.textContent = '';
        warningEl.style.display = 'none';
    }
    syncResetButton();
}
function syncResetButton(){
    const btn = document.getElementById('emergency-btn');
    if(!btn) return;
    btn.className = 'btn-reset';
    if(isEmergencyActive){
        btn.classList.add('active');
        btn.textContent = 'NOTAUS RESET';
        btn.disabled = false;
        btn.style.opacity = '1';
        return;
    }
    if(counterResetInFlight){
        btn.textContent = 'RESET...';
        btn.disabled = true;
        btn.style.opacity = '0.7';
        return;
    }
    if(resetHoldTimer){
        btn.classList.add('holding');
        btn.textContent = 'HALTEN...';
        btn.disabled = false;
        btn.style.opacity = '1';
        return;
    }
    btn.textContent = hasStickyWarning ? 'WARNUNG RESET' : 'ZAEHLER RESET 3S';
    btn.disabled = false;
    btn.style.opacity = '1';
}
function syncEmergencyState(active){
    isEmergencyActive = !!active;
    const stateEl = document.getElementById('emergency-state');
    if(stateEl){
        stateEl.textContent = isEmergencyActive ? 'AKTIV' : 'FREI';
        stateEl.className = isEmergencyActive ? 'status-emergency' : 'status-ok';
    }
    syncResetButton();
    syncFillButton();
}
function cancelResetHold(showHint){
    if(resetHoldTimer){
        clearTimeout(resetHoldTimer);
        resetHoldTimer = null;
    }
    syncResetButton();
    if(showHint && !isEmergencyActive && !resetHoldTriggered && !counterResetInFlight){
        showMsg('dashboard', hasStickyWarning ? 'Kurzer Druck loescht Warnung, 3 Sekunden halten fuer Zaehler-Reset' : 'RESET 3 Sekunden halten fuer Zaehler-Reset', false);
    }
}
function resetWarnings(){
    if(counterResetInFlight) return;
    counterResetInFlight = true;
    syncResetButton();
    fetch('/api/warnings/reset', {method: 'POST'})
        .then(r => r.json())
        .then(d => {
            updateDashboard(true);
            showMsg('dashboard', d.message || 'Warnmeldungen zurueckgesetzt', false);
        })
        .catch(() => showMsg('dashboard', 'Warnungs-Reset fehlgeschlagen', true))
        .finally(() => {
            counterResetInFlight = false;
            syncResetButton();
        });
}
function resetSensor(){
    if(counterResetInFlight) return;
    fetch('/api/sensor/reset', {method: 'POST'})
        .then(r => r.json())
        .then(d => {
            updateDashboard(true);
            showMsg('dashboard', d.message || 'Sensor-Reinit angefordert', false);
        })
        .catch(() => showMsg('dashboard', 'Sensor-Reinit fehlgeschlagen', true));
}
function triggerCounterReset(){
    resetHoldTriggered = true;
    counterResetInFlight = true;
    syncResetButton();
    fetch('/api/counters/reset', {method: 'POST'})
        .then(r => r.json())
        .then(d => {
            if(d.status !== 'OK') throw new Error(d.message || 'Reset fehlgeschlagen');
            updateDashboard(true);
            showMsg('dashboard', d.message || 'Zaehler zurueckgesetzt', false);
        })
        .catch(e => showMsg('dashboard', e.message || 'Reset fehlgeschlagen', true))
        .finally(() => {
            counterResetInFlight = false;
            syncResetButton();
            setTimeout(() => { resetHoldTriggered = false; }, 250);
        });
}
function startResetHold(ev){
    if(ev) ev.preventDefault();
    if(isEmergencyActive || counterResetInFlight || resetHoldTimer) return;
    resetHoldTriggered = false;
    resetHoldTimer = setTimeout(() => {
        resetHoldTimer = null;
        syncResetButton();
        triggerCounterReset();
    }, 3000);
    syncResetButton();
}
function setupResetButton(){
    const btn = document.getElementById('emergency-btn');
    if(!btn) return;
    btn.addEventListener('pointerdown', startResetHold);
    btn.addEventListener('pointerup', () => cancelResetHold(true));
    btn.addEventListener('pointerleave', () => cancelResetHold(false));
    btn.addEventListener('pointercancel', () => cancelResetHold(false));
    btn.addEventListener('contextmenu', ev => ev.preventDefault());
    syncResetButton();
}
function switchTab(evt, t){
  document.querySelectorAll('.tab-content').forEach(e => e.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(e => e.classList.remove('active'));
  document.getElementById(t).classList.add('active');
    if(evt && evt.target) evt.target.classList.add('active');
  if(t==='settings') loadSettings();
  if(t==='wifi') loadWiFi();
  if(t==='diagnostics'){
      setDefaultOtaUrl();
      loadDiagnostics(true);
  } else {
      clearDiagnosticsRefresh();
  }
}
function showMsg(tabId, text, isErr){
  const el = document.getElementById('msg-'+tabId);
  el.textContent = text;
  el.className = 'msg ' + (isErr ? 'error' : 'success');
  el.style.display = 'block';
  setTimeout(() => el.style.display = 'none', 4000);
}
function updateDashboard(force){
    if(dashboardPollInFlight && !force) return;
    dashboardPollInFlight = true;
    fetch('/api/status?t=' + Date.now(), {cache: 'no-store'}).then(r => {
    if(!r.ok) throw new Error('API error: ' + r.status);
    return r.json();
    }).then(d => {
    const sensors = d.sensors || {};
    const valve = d.valve || {};
    const system = d.system || {};
    isTankFull = !!sensors.tank_full;
    const timestamp = Number(d.timestamp || 0);
    if(timestamp > 0 && timestamp === lastStatusTimestamp){
        stagnantStatusCount++;
    } else {
        stagnantStatusCount = 0;
        lastStatusTimestamp = timestamp;
    }
    const lv = sensors.tank_level_cm || 0;
        const full = Math.max(0, Math.min(100, Number((sensors.fill_percent ?? sensors.tank_full) || 0)));
    document.getElementById('level').textContent = lv;
    document.getElementById('percent').textContent = full.toFixed(0) + '%';
    document.getElementById('bar-fill').style.width = full + '%';
    syncValveIndicator(valve.state === 'OPEN');
    document.getElementById('status').textContent = d.status;
        document.getElementById('app-version').textContent = system.app_version ? system.app_version : '-';
    document.getElementById('open-count').textContent = Number((valve.trigger_count ?? valve.open_count) || 0).toFixed(0);
    document.getElementById('emergency-count').textContent = Number(valve.emergency_trigger_count || 0).toFixed(0);
    document.getElementById('total-liters').textContent = Number(valve.total_liters || 0).toFixed(2) + ' L';
    document.getElementById('total-time').textContent = Math.floor(Number(valve.total_open_time_ms || 0) / 1000) + ' s';
        document.getElementById('dash-wifi').textContent = system.wifi_connected ? 'Verbunden' : 'Offline';
    syncStackWarning(system.stack_warning ? system.stack_warning_message : '');
    isFilling = !!valve.manual_fill_active;
    syncEmergencyState(!!d.emergency);
    const reasonEl = document.getElementById('emergency-reason');
    const diagEl = document.getElementById('sensor-diag');
    if(diagEl){
        const stale = !!sensors.stale;
        const invalid = Number(sensors.invalid_read_count || 0);
        const fallback = Number(sensors.fallback_reuse_count || 0);
        const raw = Number(sensors.last_raw_mm || 0);
        const freezeHint = stagnantStatusCount >= 8 ? ' | Polling unveraendert' : '';
        diagEl.className = stale ? 'diag-box diag-stale' : 'diag-box diag-live';
        diagEl.textContent = 'Sensor ' + (stale ? 'STALE' : 'LIVE') +
            ' | raw=' + raw + 'mm | invalid=' + invalid + ' | fallback=' + fallback + freezeHint;
    }
    const cpu0El = document.getElementById('cpu-core0');
    const cpu1El = document.getElementById('cpu-core1');
    const cpuTopTaskEl = document.getElementById('cpu-top-task');
    const cpuTopTaskPctEl = document.getElementById('cpu-top-task-pct');
    const cpuTaskCountEl = document.getElementById('cpu-task-count');
    if(cpu0El) cpu0El.textContent = Number(system.cpu_core0_percent || 0).toFixed(0) + ' %';
    if(cpu1El) cpu1El.textContent = Number(system.cpu_core1_percent || 0).toFixed(0) + ' %';
    if(cpuTopTaskEl) cpuTopTaskEl.textContent = system.cpu_top_task ? system.cpu_top_task : '-';
    if(cpuTopTaskPctEl) cpuTopTaskPctEl.textContent = Number(system.cpu_top_task_percent || 0).toFixed(0) + ' %';
    if(cpuTaskCountEl) cpuTaskCountEl.textContent = Number(system.cpu_task_count || 0).toFixed(0);
    if (d.emergency_reason) {
        reasonEl.textContent = 'Grund: ' + d.emergency_reason;
        reasonEl.style.display = 'block';
    } else {
        reasonEl.style.display = 'none';
    }
    }).catch(e => {
        console.error('updateDashboard failed:', e);
    }).finally(() => {
        dashboardPollInFlight = false;
        scheduleDashboardRefresh(DASHBOARD_REFRESH_MS);
    });
}
function fill(){
    if(isEmergencyActive){
        showMsg('dashboard', 'Notaus aktiv - erst RESET ausfuehren', true);
        return;
    }
    const nextAction = isFilling ? 'close' : 'open';
    fillActionInFlight = true;
    syncFillButton();
    fetch('/api/valve/manual', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({action: nextAction})
    }).then(async r => {
        if(!r.ok) throw new Error(await r.text() || ('API error: ' + r.status));
        return r.json();
    }).then(d => {
        isFilling = !!d.manual_fill_active;
        syncFillButton();
        updateDashboard(true);
        if(d.message) showMsg('dashboard', d.message, false);
    }).catch(e => {
        console.error('fill request failed:', e);
        updateDashboard(true);
        showMsg('dashboard', 'Ventilsteuerung fehlgeschlagen', true);
    }).finally(() => {
        fillActionInFlight = false;
        syncFillButton();
    });
}
function stop(){fetch('/api/valve/stop', {method: 'POST'}).then(() => {isFilling = false; updateDashboard(true); showMsg('dashboard', 'Ventil geschlossen', false);});}
function resetEmergency(){if(!isEmergencyActive){if(resetHoldTriggered) return; const now = Date.now(); const isDoubleTap = (now - lastResetTapMs) <= 450; lastResetTapMs = now; if(isDoubleTap){resetSensor(); return;} if(hasStickyWarning){resetWarnings(); return;} showMsg('dashboard', 'Doppeltipp: Sensor-Reinit | 3 Sekunden halten: Zaehler-Reset', false); return;} fetch('/api/emergency_stop', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'reset'})}).then(r => r.json()).then(d => {updateDashboard(true); document.getElementById('emergency-reason').style.display = 'none'; showMsg('dashboard', d.message || 'Reset ausgefuehrt', false);}).catch(() => showMsg('dashboard', 'Reset fehlgeschlagen', true));}
function loadSettings(){fetch('/api/config').then(r => r.json()).then(d => {document.getElementById('top').value = d.config.threshold_top_cm; document.getElementById('bottom').value = d.config.threshold_bottom_cm; document.getElementById('timeout').value = d.config.timeout_max_ms; document.getElementById('fill-progress-timeout').value = d.config.fill_progress_timeout_ms; document.getElementById('flow-rate').value = d.config.flow_rate_l_per_min;});}
function saveSettings(){const top = parseInt(document.getElementById('top').value, 10); const bottom = parseInt(document.getElementById('bottom').value, 10); const timeout = parseInt(document.getElementById('timeout').value, 10); const fillProgressTimeout = parseInt(document.getElementById('fill-progress-timeout').value, 10); const flowRate = parseFloat(document.getElementById('flow-rate').value); if(!Number.isFinite(top) || !Number.isFinite(bottom) || !Number.isFinite(timeout) || !Number.isFinite(fillProgressTimeout) || !Number.isFinite(flowRate)){showMsg('settings', 'Alle Felder muessen gueltige Zahlen enthalten', true); return;} if(top < 1 || top > 100 || bottom < 1 || bottom > 100){showMsg('settings', 'OBEN und UNTEN muessen zwischen 1 und 100 cm liegen', true); return;} if(top >= bottom){showMsg('settings', 'OBEN muss kleiner als UNTEN sein', true); return;} if(timeout < 1000 || fillProgressTimeout < 1000){showMsg('settings', 'Timeout-Werte muessen mindestens 1000 ms sein', true); return;} if(flowRate <= 0 || flowRate > 50){showMsg('settings', 'Durchfluss muss zwischen 0.1 und 50 L/min liegen', true); return;} const cfg = {threshold_top_cm: top, threshold_bottom_cm: bottom, timeout_max_ms: timeout, fill_progress_timeout_ms: fillProgressTimeout, flow_rate_l_per_min: flowRate}; settingsSaveInFlight = true; syncSaveButton(); fetch('/api/config', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(cfg)}).then(async r => {if(!r.ok) throw new Error(await r.text() || ('API error: ' + r.status)); return r.json();}).then(() => {showMsg('settings', 'Einstellungen gespeichert', false); updateDashboard(true);}).catch(e => showMsg('settings', 'Fehler: ' + e.message, true)).finally(() => {settingsSaveInFlight = false; syncSaveButton();});}
function loadWiFi(){fetch('/api/wifi/status').then(r => {if(!r.ok) throw new Error('API error: '+r.status); return r.json();}).then(d => {const c=d.wifi&&d.wifi.connected; document.getElementById('wifi-con').textContent=c?'Verbunden':'Getrennt'; document.getElementById('wifi-con').style.color=c?'#4caf50':'#f44336'; document.getElementById('wifi-ssid').textContent = (d.wifi && d.wifi.ssid) ? d.wifi.ssid : '-'; document.getElementById('wifi-rssi').textContent = (d.wifi && d.wifi.rssi) ? (d.wifi.rssi + ' dBm') : '-'; document.getElementById('wifi-ip').textContent = (d.wifi && d.wifi.ip) ? d.wifi.ip : '-';}).catch(e => {console.error('loadWiFi failed:', e); document.getElementById('wifi-con').textContent='Fehler'; showMsg('wifi', 'WiFi API Fehler', true);});}
function connectWiFi(){const s = document.getElementById('new-ssid').value; const p = document.getElementById('new-pass').value; if(!s||!p) {showMsg('wifi', 'SSID und Pass erforderlich', true); return;} fetch('/api/wifi/config', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({ssid: s, password: p})}).then(r => {if(!r.ok) throw new Error('API error: '+r.status); return r.json();}).then(d => {showMsg('wifi', 'WiFi Update gesendet', false); document.getElementById('new-ssid').value = ''; document.getElementById('new-pass').value = ''; setTimeout(loadWiFi, 2000);}).catch(e => {console.error('connectWiFi failed:', e); showMsg('wifi', 'Fehler: '+e.message, true);});}
function reset(){if(confirm('System wirklich neustarten?')) fetch('/api/system/reset', {method: 'POST'}).then(() => showMsg('wifi', 'Neustart...', false)).catch(e => showMsg('wifi', 'Fehler', true));}
syncFillButton();
syncValveIndicator(false);
syncSaveButton();
setupResetButton();
updateDashboard();
</script>
</body></html>)html";
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

// ============================================================================
// Phase 1: Sensor Task (VL6150X/VL6180X Distance Measurement)
// ============================================================================

// ============================================================================
// VL6150X / VL6180X ToF Sensor Port (TOF050C)
// Based on: Adafruit VL6180X driver for Arduino
// This code now supports ST FlightSense VL6150X-compatible sensors on I2C addr 0x29
// ============================================================================

// --- VL6180X Register Map ---
#define SYSRANGE_START                              0x0018
#define SYSRANGE_PART_TO_PART_RANGE_OFFSET          0x0024
#define SYSALS_START                                0x0038
#define SYSALS_ANALOGUE_GAIN                        0x003F
#define SYSALS_INTEGRATION_PERIOD_HI                0x0040
#define SYSALS_INTEGRATION_PERIOD_LO                0x0041
#define SYSTEM_INTERRUPT_CONFIG                     0x0014
#define SYSTEM_INTERRUPT_CLEAR                      0x0015
#define SYSTEM_FRESH_OUT_OF_RESET                   0x0016
#define RESULT_RANGE_STATUS                         0x004D
#define RESULT_INTERRUPT_STATUS_GPIO                0x004F
#define RESULT_ALS_VAL                              0x0050
#define RESULT_RANGE_VAL                            0x0062
#define IDENTIFICATION_MODEL_ID                     0x0000
#define IDENTIFICATION_REVISION_ID                  0x0001
#define SLAVE_DEVICE_ADDRESS                        0x0212
#define SYSRANGE__INTERMEASUREMENT_PERIOD           0x001B

// Module-level state
static int unused_tof_state_marker = 0;

#define I2C_TRANSFER_TIMEOUT_MS 1000
#define I2C_SCAN_TIMEOUT_MS 200
#define SENSOR_AUTO_REINIT_FAIL_THRESHOLD 20
#define SENSOR_AUTO_REINIT_COOLDOWN_MS 10000
static esp_err_t tof_i2c_write(const uint8_t *data, size_t len)
{
    esp_err_t ret = i2c_master_transmit(vl53l0x_dev_handle, data, len, I2C_TRANSFER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ToF I2C write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t tof_i2c_write_read(const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len)
{
    esp_err_t ret = i2c_master_transmit_receive(vl53l0x_dev_handle, write_data, write_len, read_data, read_len, I2C_TRANSFER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ToF I2C write/read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

// --- I2C Low-Level Helpers ---

static esp_err_t vl53l0x_write_reg(uint16_t reg, uint8_t value)
{
    uint8_t data[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value};
    return tof_i2c_write(data, sizeof(data));
}

static esp_err_t vl53l0x_write_reg16(uint16_t reg, uint16_t value)
{
    uint8_t data[4] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return tof_i2c_write(data, sizeof(data));
}

static esp_err_t vl53l0x_write_multi(uint16_t reg, const uint8_t *src, uint8_t count)
{
    uint8_t buf[2 + count];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(&buf[2], src, count);
    return tof_i2c_write(buf, count + 2);
}

static uint8_t vl53l0x_read_reg(uint16_t reg)
{
    uint8_t value = 0;
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    esp_err_t ret = tof_i2c_write_read(addr, sizeof(addr), &value, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ToF read reg 0x%04X failed: %s", reg, esp_err_to_name(ret));
        return 0;
    }
    return value;
}

static uint16_t vl53l0x_read_reg16(uint16_t reg)
{
    uint8_t data[2] = {0};
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    esp_err_t ret = tof_i2c_write_read(addr, sizeof(addr), data, 2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ToF read reg16 0x%04X failed: %s", reg, esp_err_to_name(ret));
        return 0;
    }
    return ((uint16_t)data[0] << 8) | data[1];
}

static bool vl53l0x_read_reg_retry(uint16_t reg, uint8_t *value, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
        esp_err_t ret = tof_i2c_write_read(addr, sizeof(addr), value, 1);
        if (ret == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "ToF read reg 0x%04X attempt %d failed: %s", reg, i + 1, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static esp_err_t vl53l0x_read_multi(uint16_t reg, uint8_t *dst, uint8_t count)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return tof_i2c_write_read(addr, sizeof(addr), dst, count);
}

// --- I2C Bus Scan (optimized) ---

static void i2c_scan_bus(void)
{
    ESP_LOGI(TAG, "🔍 I2C bus scan (0x03-0x77)...");
    int found = 0;
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        esp_task_wdt_reset();
        esp_err_t ret = i2c_master_probe(i2c_bus_handle, addr, I2C_SCAN_TIMEOUT_MS);
        if (ret == ESP_OK) {
            const char *name = (addr == 0x29) ? " (TOF sensor)" :
                               (addr == 0x52) ? " (EEPROM)" :
                               (addr == 0x68) ? " (MPU6050)" :
                               (addr == 0x76) ? " (BME280)" : "";
            ESP_LOGI(TAG, "   ✅ 0x%02X%s", addr, name);
            found++;
        }
    }
    ESP_LOGI(TAG, "   Scan: %d device(s) found", found);
}

// --- VL6180X Helper Routines ---

// --- VL6180X init sequence ---

static bool vl53l0x_sensor_ready(void)
{
    esp_err_t probe = i2c_master_probe(i2c_bus_handle, TOF_SENSOR_ADDR, I2C_SCAN_TIMEOUT_MS);
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "❌ ToF sensor I2C probe failed at 0x%02X: %s", TOF_SENSOR_ADDR, esp_err_to_name(probe));
        ESP_LOGE(TAG, "   Verify wiring, power, pull-ups, and that the sensor is not held in reset.");
        return false;
    }

    uint8_t id = 0;
    if (!vl53l0x_read_reg_retry(IDENTIFICATION_MODEL_ID, &id, 3)) {
        ESP_LOGE(TAG, "❌ ToF sensor model register read failed after retries.");
        ESP_LOGE(TAG, "   Device acked at 0x%02X but did not return a valid model ID.", TOF_SENSOR_ADDR);
        return false;
    }

    if (id == 0xB4) {
        uint8_t rev = 0;
        if (!vl53l0x_read_reg_retry(IDENTIFICATION_REVISION_ID, &rev, 3)) {
            ESP_LOGW(TAG, "⚠️  ToF sensor revision read failed, continuing with model OK");
        }
        ESP_LOGI(TAG, "✅ VL6150X/VL6180X FOUND (Model 0x%02X, Rev 0x%02X)", id, rev);
        return true;
    }

    ESP_LOGE(TAG, "❌ ToF sensor not found (got 0x%02X)", id);
    ESP_LOGE(TAG, "   I2C device responded at 0x%02X, but the model register is invalid.", TOF_SENSOR_ADDR);
    ESP_LOGE(TAG, "   Possible causes: wrong sensor wiring/orientation, bad ground reference, or I2C bus noise.");
    return false;
}

static void vl53l0x_load_settings(void)
{
    // Recommended VL6180X initialization from Adafruit driver
    vl53l0x_write_reg(0x0207, 0x01);
    vl53l0x_write_reg(0x0208, 0x01);
    vl53l0x_write_reg(0x0096, 0x00);
    vl53l0x_write_reg(0x0097, 0xFD);
    vl53l0x_write_reg(0x00E3, 0x00);
    vl53l0x_write_reg(0x00E4, 0x04);
    vl53l0x_write_reg(0x00E5, 0x02);
    vl53l0x_write_reg(0x00E6, 0x01);
    vl53l0x_write_reg(0x00E7, 0x03);
    vl53l0x_write_reg(0x00F5, 0x02);
    vl53l0x_write_reg(0x00D9, 0x05);
    vl53l0x_write_reg(0x00DB, 0xCE);
    vl53l0x_write_reg(0x00DC, 0x03);
    vl53l0x_write_reg(0x00DD, 0xF8);
    vl53l0x_write_reg(0x009F, 0x00);
    vl53l0x_write_reg(0x00A3, 0x3C);
    vl53l0x_write_reg(0x00B7, 0x00);
    vl53l0x_write_reg(0x00BB, 0x3C);
    vl53l0x_write_reg(0x00B2, 0x09);
    vl53l0x_write_reg(0x00CA, 0x09);
    vl53l0x_write_reg(0x0198, 0x01);
    vl53l0x_write_reg(0x01B0, 0x17);
    vl53l0x_write_reg(0x01AD, 0x00);
    vl53l0x_write_reg(0x00FF, 0x05);
    vl53l0x_write_reg(0x0100, 0x05);
    vl53l0x_write_reg(0x0199, 0x05);
    vl53l0x_write_reg(0x01A6, 0x1B);
    vl53l0x_write_reg(0x01AC, 0x3E);
    vl53l0x_write_reg(0x01A7, 0x1F);
    vl53l0x_write_reg(0x0030, 0x00);
    vl53l0x_write_reg(SYSTEM_INTERRUPT_CONFIG, 0x10);
    vl53l0x_write_reg(0x010A, 0x30);
    vl53l0x_write_reg(0x003F, 0x46);
    vl53l0x_write_reg(0x0031, 0xFF);
    vl53l0x_write_reg(0x0041, 0x63);
    vl53l0x_write_reg(0x002E, 0x01);
    vl53l0x_write_reg(SYSRANGE__INTERMEASUREMENT_PERIOD, 0x09);
    vl53l0x_write_reg(0x003E, 0x31);
    vl53l0x_write_reg(0x0014, 0x24);
}

static esp_err_t vl53l0x_init(void)
{
    ESP_LOGI(TAG, "🔧 VL6150X/VL6180X init...");

    uint8_t id = vl53l0x_read_reg(IDENTIFICATION_MODEL_ID);
    if (id != 0xB4) {
        ESP_LOGE(TAG, "Model ID mismatch: 0x%02X", id);
        return ESP_FAIL;
    }

    // Always apply the recommended initialization register sequence to ensure
    // consistent VL6180X behavior after power-up or from a preserved state.
    vl53l0x_load_settings();
    vl53l0x_write_reg(SYSTEM_FRESH_OUT_OF_RESET, 0x00);
    ESP_LOGI(TAG, "   VL6180X init sequence applied");

    return ESP_OK;
}

// --- VL6180X single-shot range read ---

static uint16_t vl53l0x_read_single_mm(void)
{
    // Trigger a single-shot VL6180X range measurement.
    vl53l0x_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x07);
    vl53l0x_write_reg(SYSRANGE_START, 0x01);

    uint32_t start = esp_timer_get_time() / 1000;
    while ((vl53l0x_read_reg(RESULT_INTERRUPT_STATUS_GPIO) & 0x04) == 0) {
        if ((esp_timer_get_time() / 1000 - start) > 500) {
            ESP_LOGW(TAG, "VL6180X read timeout");
            return 65535;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint8_t range = vl53l0x_read_reg(RESULT_RANGE_VAL);
    vl53l0x_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x07);
    return range;
}

/**
 * @brief Task: Read sensor and monitor fill level
 * Priority: HIGH (prevents tank overflow)
 */
static void sensor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🌡️  Sensor task started");
    
    // Wait for task to be registered with watchdog
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Wait for I2C to be ready
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // DIAGNOSTIC: Scan I2C bus for all devices
    ESP_LOGI(TAG, "🔍 Scanning I2C bus...");
    i2c_scan_bus();
    
    vTaskDelay(pdMS_TO_TICKS(500));  // Give time to read logs
    
    // Try to initialize VL6150X/VL6180X sensor (real hardware)
    bool sensor_available = false;
    
    ESP_LOGI(TAG, "🔧 Attempting TOF sensor detection...");
    if (vl53l0x_sensor_ready()) {
        ESP_LOGI(TAG, "✅ Sensor detected! Now initializing...");
        // Attempt full initialization sequence
        esp_err_t init_result = vl53l0x_init();
        if (init_result == ESP_OK) {
            sensor_available = true;
            ESP_LOGI(TAG, "✅ TOF sensor initialized and ready!");
            vTaskDelay(pdMS_TO_TICKS(100));  // Wait for first measurement
        } else {
            ESP_LOGE(TAG, "❌ Sensor detected but init FAILED (code %d) - using simulation", init_result);
        }
    } else {
        ESP_LOGE(TAG, "⚠️  TOF sensor NOT DETECTED - EMERGENCY STOP");
        ESP_LOGE(TAG, "   If the I2C scan did not show 0x29, verify wiring, pull-ups and sensor power.");
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        trigger_emergency_stop("TOF sensor not detected");
        xSemaphoreGive(sys_state_mutex);
    }
    
    ESP_LOGI(TAG, "🚀 Sensor mode: %s", sensor_available ? "REAL HARDWARE" : "EMERGENCY (no sensor)");
    
    uint16_t distance_samples[SENSOR_SAMPLES] = {0};
    int sample_idx = 0;
    uint32_t distance_sum = 0;
    uint32_t sample_count = 0;
    uint32_t stable_counter = 0;
    uint32_t fail_count = 0;
    uint64_t last_auto_reinit_ms = 0;
    
    while (1) {
        // Feed watchdog
        esp_task_wdt_reset();

        if (sensor_reinit_requested) {
            sensor_reinit_requested = false;
            ESP_LOGW(TAG, "🔄 Sensor reinit requested via UI reset double-tap");
            bool was_available = sensor_available;
            esp_err_t reinit_result = vl53l0x_init();
            if (reinit_result == ESP_OK) {
                sensor_available = true;
                fail_count = 0;
                memset(distance_samples, 0, sizeof(distance_samples));
                sample_idx = 0;
                distance_sum = 0;
                sample_count = 0;
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_state.sensor_data_stale = false;
                sys_state.sensor_last_raw_mm = 0;
                xSemaphoreGive(sys_state_mutex);
                ESP_LOGI(TAG, "✅ Sensor reinit successful");
            } else {
                sensor_available = was_available;
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                if (!sensor_available) {
                    sys_state.sensor_data_stale = true;
                }
                xSemaphoreGive(sys_state_mutex);
                ESP_LOGE(TAG, "❌ Sensor reinit failed: %s", esp_err_to_name(reinit_result));
            }
        }

        if (!sensor_available) {
            uint64_t now_ms = esp_timer_get_time() / 1000;
            if (last_auto_reinit_ms == 0 || (now_ms - last_auto_reinit_ms) >= SENSOR_AUTO_REINIT_COOLDOWN_MS) {
                sensor_reinit_requested = true;
                last_auto_reinit_ms = now_ms;
                ESP_LOGW(TAG, "🔁 Sensor unavailable - scheduling periodic reinit attempt");
            }
        }
        
        // Read sensor - either real or simulated
        uint16_t distance_mm = 0;
        
        if (sensor_available) {
            // Read from REAL sensor (Pololu single-shot)
            distance_mm = vl53l0x_read_single_mm();
            
            // 65535 = timeout, 0 = invalid
            if (distance_mm == 65535 || distance_mm == 0 || distance_mm > 4000) {
                fail_count++;
                if ((fail_count % 10) == 0) {
                    ESP_LOGW(TAG, "❌ Invalid reading: %d mm (%d fails)", distance_mm, fail_count);
                }

                uint64_t now_ms = esp_timer_get_time() / 1000;
                if (fail_count >= SENSOR_AUTO_REINIT_FAIL_THRESHOLD &&
                    (last_auto_reinit_ms == 0 || (now_ms - last_auto_reinit_ms) >= SENSOR_AUTO_REINIT_COOLDOWN_MS)) {
                    sensor_reinit_requested = true;
                    last_auto_reinit_ms = now_ms;
                    fail_count = 0;
                    ESP_LOGW(TAG,
                        "🔁 Auto sensor reinit requested after consecutive invalid readings");
                }

                // Use last good value from filter buffer
                if (distance_samples[0] > 0) {
                    distance_mm = distance_samples[(sample_idx + SENSOR_SAMPLES - 1) % SENSOR_SAMPLES];
                } else {
                    distance_mm = 1500;  // Fallback
                }

                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_state.sensor_invalid_read_count++;
                sys_state.sensor_fallback_reuse_count++;
                sys_state.sensor_data_stale = true;
                sys_state.sensor_last_raw_mm = 0;
                xSemaphoreGive(sys_state_mutex);
            } else {
                fail_count = 0;
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                sys_state.sensor_last_raw_mm = distance_mm;
                sys_state.sensor_data_stale = false;
                xSemaphoreGive(sys_state_mutex);
            }
            
            // Check if distance > 30cm (300mm), emergency stop
            if (distance_mm > 3000) {  // 30cm = 300mm, but allow some margin
                system_state_t state_snapshot;
                get_system_state_snapshot(&state_snapshot);
                if (!state_snapshot.emergency_stop_active) {
                    trigger_emergency_stop("Sensor reading too high (>30cm), possible sensor failure");
                    get_system_state_snapshot(&state_snapshot);
                    ESP_LOGE(TAG, "🚨 EMERGENCY STOP - %s", state_snapshot.emergency_stop_reason);
                }
            }
        } else {
            // No sensor - emergency already triggered
            vTaskDelay(pdMS_TO_TICKS(TASK_SENSOR_INTERVAL_MS));
            continue;
        }
        
        // Moving average filter (5-sample buffer for noise reduction)
        distance_sum -= distance_samples[sample_idx];
        distance_samples[sample_idx] = distance_mm;
        distance_sum += distance_mm;
        sample_idx = (sample_idx + 1) % SENSOR_SAMPLES;

        if (sample_count < SENSOR_SAMPLES) {
            sample_count++;
        }

        uint16_t distance_filtered_mm = (uint16_t)(distance_sum / sample_count);
        uint16_t distance_cm = distance_filtered_mm / 10;
        
        // Update system state
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        int prev_distance = sys_state.sensor_distance_cm;
        sys_state.sensor_distance_cm = distance_cm;
        sys_state.last_update_timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
        if (!sensor_available) {
            sys_state.sensor_data_stale = true;
        }
        xSemaphoreGive(sys_state_mutex);
        
        // Log significant changes
        if (ABS(distance_cm - prev_distance) > 2 || (stable_counter % 20) == 0) {
            uint32_t threshold_top;
            uint32_t threshold_bottom;

            xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
            threshold_top = sys_state.threshold_top;
            threshold_bottom = sys_state.threshold_bottom;
            xSemaphoreGive(sys_state_mutex);

            int percent = calculate_fill_percent(distance_cm, threshold_top, threshold_bottom);
            
            const char *source = sensor_available ? "🌡️ REAL" : "📊 SIM";
            ESP_LOGI(TAG, "%s Tank Level: %3d cm (%3d pct) | Range: OBEN=%d, UNTEN=%d", 
                     source, distance_cm, percent, threshold_top, threshold_bottom);
        }
        
        stable_counter++;
        vTaskDelay(pdMS_TO_TICKS(TASK_SENSOR_INTERVAL_MS));
    }
}

// ============================================================================
// Phase 1: Valve Control Task
// ============================================================================

/**
 * @brief Task: Control solenoid valve and manage filling timeout
 * Priority: HIGH (critical for safety)
 * 
 * Fill Logic:
 * - When tank level drops below UNTEN: Open valve and fill
 * - When tank level reaches OBEN: Close valve
 * - If filling exceeds TIMEOUT_MAX: Close valve as safety measure
 */
static void valve_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🚰 Valve task started");
    
    // Wait for task to be registered with watchdog
    vTaskDelay(pdMS_TO_TICKS(100));
    
    uint64_t fill_start_time_ms = 0;
    uint64_t last_progress_time_ms = 0;
    uint64_t last_no_progress_log_ms = 0;
    bool filling = false;
    bool manual_mode = false;
    uint16_t last_distance_cm = 0;
    uint16_t progress_reference_distance = 0;
    uint16_t progress_candidate_distance = 0;
    uint8_t progress_confirmation_count = 0;
    int last_tank_state = 0;  // 0=unknown, 1=full, 2=filling, 3=empty
    
    while (1) {
        // Feed watchdog - CRITICAL for safety
        esp_task_wdt_reset();
        system_state_t state_snapshot;
        get_system_state_snapshot(&state_snapshot);
        
        int current_tank_state = 0;
        
        // Determine tank state based on sensor readings
        if (state_snapshot.sensor_distance_cm <= state_snapshot.threshold_top) {
            current_tank_state = 1;  // FULL
        } else if (state_snapshot.sensor_distance_cm >= state_snapshot.threshold_bottom) {
            current_tank_state = 3;  // EMPTY
        } else {
            current_tank_state = 2;  // FILLING
        }

        if (state_snapshot.manual_fill_active && current_tank_state == 1) {
            set_manual_fill_active(false);
            state_snapshot.manual_fill_active = false;
        }

        if (filling && !state_snapshot.valve_state) {
            finalize_active_valve_session(esp_timer_get_time() / 1000);
            filling = false;
            manual_mode = false;
            last_progress_time_ms = 0;
            last_no_progress_log_ms = 0;
            last_distance_cm = 0;
            progress_reference_distance = 0;
            progress_candidate_distance = 0;
            progress_confirmation_count = 0;
            ESP_LOGI(TAG, "🚰 Valve CLOSED - session finalized after external stop");
        }

        if (filling && manual_mode && !state_snapshot.manual_fill_active) {
            gpio_set_level(GPIO_VALVE_CONTROL, 0);
            set_valve_and_manual_state(false, false);
            finalize_active_valve_session(esp_timer_get_time() / 1000);
            filling = false;
            manual_mode = false;
            last_progress_time_ms = 0;
            last_no_progress_log_ms = 0;
            last_distance_cm = 0;
            progress_reference_distance = 0;
            progress_candidate_distance = 0;
            progress_confirmation_count = 0;
            ESP_LOGI(TAG, "🚰 Valve CLOSED - manual fill stopped");
        }

        if (state_snapshot.manual_fill_active && !filling && !state_snapshot.emergency_stop_active && current_tank_state != 1) {
            gpio_set_level(GPIO_VALVE_CONTROL, 1);
            filling = true;
            manual_mode = true;
            fill_start_time_ms = esp_timer_get_time() / 1000;
            last_progress_time_ms = fill_start_time_ms;
            last_no_progress_log_ms = fill_start_time_ms;
            begin_valve_session(fill_start_time_ms, true);
            progress_reference_distance = state_snapshot.sensor_distance_cm;
            progress_candidate_distance = state_snapshot.sensor_distance_cm;
            last_distance_cm = state_snapshot.sensor_distance_cm;
            progress_confirmation_count = 0;
            ESP_LOGI(TAG, "🚰 Valve OPENED - manual fill requested");
        }
        
        // Handle state transitions
        if (current_tank_state != last_tank_state) {
            switch (current_tank_state) {
                case 1:  // Tank FULL
                    if (filling) {
                        gpio_set_level(GPIO_VALVE_CONTROL, 0);  // Close valve
                        set_valve_and_manual_state(false, false);
                        finalize_active_valve_session(esp_timer_get_time() / 1000);
                        filling = false;
                        manual_mode = false;
                        last_progress_time_ms = 0;
                        last_no_progress_log_ms = 0;
                        last_distance_cm = 0;
                        progress_reference_distance = 0;
                        progress_candidate_distance = 0;
                        progress_confirmation_count = 0;
                        ESP_LOGI(TAG, "🚰 Valve CLOSED - Tank is FULL (reached OBEN threshold)");
                    }
                    break;
                    
                case 3:  // Tank EMPTY
                    break;
            }
            last_tank_state = current_tank_state;
        }

        if (current_tank_state == 3 && !filling && !state_snapshot.emergency_stop_active && !state_snapshot.manual_fill_active && !state_snapshot.user_fill_halt) {
            gpio_set_level(GPIO_VALVE_CONTROL, 1);  // Open valve
            filling = true;
            manual_mode = false;
            fill_start_time_ms = esp_timer_get_time() / 1000;
            last_progress_time_ms = fill_start_time_ms;
            last_no_progress_log_ms = fill_start_time_ms;
            begin_valve_session(fill_start_time_ms, false);
            progress_reference_distance = state_snapshot.sensor_distance_cm;
            progress_candidate_distance = state_snapshot.sensor_distance_cm;
            last_distance_cm = state_snapshot.sensor_distance_cm;
            progress_confirmation_count = 0;
            ESP_LOGI(TAG, "🚰 Valve OPENED - Tank is EMPTY (below UNTEN threshold) - FILLING STARTED");
        }
        
        // Timeout protection: if filling exceeds max timeout
        if (filling) {
            uint64_t now_ms = esp_timer_get_time() / 1000;
            uint16_t current_distance = state_snapshot.sensor_distance_cm;

            if (last_distance_cm > 0 && current_distance + FILL_PROGRESS_MIN_DELTA_CM <= last_distance_cm) {
                if (current_distance < progress_candidate_distance) {
                    progress_candidate_distance = current_distance;
                }
                if (progress_confirmation_count < UINT8_MAX) {
                    progress_confirmation_count++;
                }

                // Any real decrease in measured distance resets the progress timeout.
                last_progress_time_ms = now_ms;
                last_no_progress_log_ms = now_ms;

                if (progress_confirmation_count >= FILL_PROGRESS_CONFIRM_SAMPLES) {
                    progress_reference_distance = progress_candidate_distance;
                    progress_candidate_distance = progress_reference_distance;
                    progress_confirmation_count = 0;
                    ESP_LOGI(TAG, "📉 Fill progress confirmed: %u cm", progress_reference_distance);
                }
            } else {
                if (current_distance < progress_candidate_distance) {
                    progress_candidate_distance = current_distance;
                }

                if ((now_ms - last_progress_time_ms) > state_snapshot.fill_progress_timeout_ms) {
                    gpio_set_level(GPIO_VALVE_CONTROL, 0);
                    set_valve_and_manual_state(false, false);
                    trigger_emergency_stop(manual_mode
                        ? "No fill progress during manual fill: distance did not decrease sufficiently within timeout"
                        : "No fill progress: distance did not decrease sufficiently within timeout");
                    finalize_active_valve_session(now_ms);
                    filling = false;
                    manual_mode = false;
                    last_progress_time_ms = 0;
                    last_no_progress_log_ms = 0;
                    progress_reference_distance = 0;
                    progress_candidate_distance = 0;
                    progress_confirmation_count = 0;
                    ESP_LOGE(TAG, "🚨 EMERGENCY STOP - %s", sys_state.emergency_stop_reason);
                    last_tank_state = current_tank_state;
                    vTaskDelay(pdMS_TO_TICKS(TASK_VALVE_CHECK_MS));
                    continue;
                } else if ((now_ms - last_no_progress_log_ms) >= 1000) {
                    uint64_t stalled_ms = now_ms - last_progress_time_ms;
                    uint64_t remaining_ms = (stalled_ms >= state_snapshot.fill_progress_timeout_ms)
                        ? 0
                        : (state_snapshot.fill_progress_timeout_ms - stalled_ms);
                    ESP_LOGW(TAG,
                        "⏳ No fill progress (%s): current=%u cm ref=%u cm stalled=%llu ms remaining=%llu ms",
                        manual_mode ? "manual" : "auto",
                        current_distance,
                        progress_reference_distance,
                        (unsigned long long)stalled_ms,
                        (unsigned long long)remaining_ms);
                    last_no_progress_log_ms = now_ms;
                }
            }

            last_distance_cm = current_distance;
            uint64_t elapsed_ms = now_ms - fill_start_time_ms;
            if (elapsed_ms > state_snapshot.timeout_max) {
                gpio_set_level(GPIO_VALVE_CONTROL, 0);  // Close valve
                set_valve_and_manual_state(false, false);
                finalize_active_valve_session(now_ms);
                filling = false;
                manual_mode = false;
                last_progress_time_ms = 0;
                last_no_progress_log_ms = 0;
                progress_reference_distance = 0;
                progress_candidate_distance = 0;
                progress_confirmation_count = 0;
                last_distance_cm = 0;
                ESP_LOGW(TAG, "⚠️  TIMEOUT! Valve CLOSED - fill time exceeded %d ms", 
                        state_snapshot.timeout_max);
                // Note: Not calling emergency stop, just safety closure
            }
        }
        
        // LED feedback: ON while opening, OFF when closed
        get_system_state_snapshot(&state_snapshot);
        if (state_snapshot.emergency_stop_active) {
            gpio_set_level(GPIO_LED_STATUS, 1);  // Red alert
        } else {
            gpio_set_level(GPIO_LED_STATUS, state_snapshot.valve_state ? 1 : 0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(TASK_VALVE_CHECK_MS));
    }
}

// ============================================================================
// Phase 3: WiFi Event Handler
// ============================================================================

/**
 * @brief WiFi Event Handler (non-blocking, retry handled by wifi_task)
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "📡 WiFi STA started - attempting to connect...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = event->reason;
        ESP_LOGW(TAG, "WiFi disconnected (reason: %d)", reason);
        record_wifi_disconnect(reason, (uint32_t)(esp_timer_get_time() / 1000));
        
        // Don't retry here - let wifi_task handle retry logic
        // This prevents blocking the event loop
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✅ WiFi STA connected!");
        ESP_LOGI(TAG, "   IP: " IPSTR, IP2STR(&event->ip_info.ip));

        mark_wifi_connected();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Device connected to AP");
    }
}

// ============================================================================
// Phase 1: WiFi Task (STA + AP Fallback)
// ============================================================================

/**
 * @brief Task: WiFi Connection Management with Retry Logic
 * 
 * Handles WiFi state transitions:
 * - STA mode: 3 connection attempts à 3 seconds
 * - Fallback: AP mode if all retries fail
 */
static void wifi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi management task started");
    
    while (1) {
        uint32_t now = esp_timer_get_time() / 1000;
        wifi_state_t wifi_snapshot;
        get_wifi_state_snapshot(&wifi_snapshot);
        
        // If already connected, just monitor
        if (wifi_snapshot.is_connected) {
            if (wifi_snapshot.ap_active) {
                set_fallback_ap_enabled(false);
            }
            vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
            continue;
        }
        
        // Not connected - check if we should retry or switch to AP mode
        if (wifi_snapshot.ap_active) {
            // Already in AP mode - wait for user to configure WiFi
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        // In STA mode but not connected
        if (wifi_snapshot.retry_count < 3) {
            // Check if 3 seconds have passed since last attempt
            uint32_t time_since_attempt = now - wifi_snapshot.last_attempt_tick;
            
            if (time_since_attempt >= 3000 || wifi_snapshot.last_attempt_tick == 0) {
                // Time to retry (or first attempt)
                set_wifi_retry_attempt_timestamp(now);
                ESP_LOGI(TAG, "🔄 WiFi connect attempt %d/3...", wifi_snapshot.retry_count + 1);
                esp_wifi_connect();
            }
        } else {
            // Max retries reached - AP is already running in APSTA mode
            ESP_LOGE(TAG, "❌ WiFi STA failed after 3 attempts - AP still active @ " AP_MODE_IP_ADDR);
            set_fallback_ap_enabled(true);
            // Stop retrying, user can enter credentials via AP
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));  // Check every second
    }
}

/**
 * @brief Start HTTP server on port 80 with REST API endpoints
 */
static httpd_handle_t start_webserver(void)
{
    ESP_LOGI(TAG, "Starting HTTP server on port %d", SERVER_PORT);
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SERVER_PORT;
    config.stack_size = TASK_STACK_SERVER;
    config.task_priority = TASK_PRIO_SERVER;
    config.max_open_sockets = MAX_OPEN_SOCKETS;
    config.max_uri_handlers = 24;
    config.core_id = TASK_CORE_NETWORK;
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "HTTP server started - registering endpoints");
        
        // Register GET / (root HTML) - CRITICAL
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_uri);
        
        // Register GET /api/status
        httpd_uri_t status_uri = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status_uri);
        
        // Register POST /api/valve/manual
        httpd_uri_t valve_uri = {
            .uri = "/api/valve/manual",
            .method = HTTP_POST,
            .handler = valve_manual_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &valve_uri);
        
        // Register POST /api/emergency_stop
        httpd_uri_t emergency_uri = {
            .uri = "/api/emergency_stop",
            .method = HTTP_POST,
            .handler = emergency_stop_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &emergency_uri);
        
        // Register GET /api/config
        httpd_uri_t config_get_uri = {
            .uri = "/api/config",
            .method = HTTP_GET,
            .handler = config_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &config_get_uri);
        
        // Register POST /api/config
        httpd_uri_t config_post_uri = {
            .uri = "/api/config",
            .method = HTTP_POST,
            .handler = config_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &config_post_uri);
        
        // Register POST /api/valve/stop
        httpd_uri_t valve_stop_uri = {
            .uri = "/api/valve/stop",
            .method = HTTP_POST,
            .handler = valve_stop_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &valve_stop_uri);
        
        // Register GET /api/wifi/status
        httpd_uri_t wifi_status_uri = {
            .uri = "/api/wifi/status",
            .method = HTTP_GET,
            .handler = wifi_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_status_uri);
        
        // Register POST /api/wifi/config
        httpd_uri_t wifi_config_uri = {
            .uri = "/api/wifi/config",
            .method = HTTP_POST,
            .handler = wifi_config_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_config_uri);

        // Register POST /api/counters/reset
        httpd_uri_t counters_reset_uri = {
            .uri = "/api/counters/reset",
            .method = HTTP_POST,
            .handler = counters_reset_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &counters_reset_uri);

        httpd_uri_t warnings_reset_uri = {
            .uri = "/api/warnings/reset",
            .method = HTTP_POST,
            .handler = warnings_reset_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &warnings_reset_uri);

        httpd_uri_t sensor_reset_uri = {
            .uri = "/api/sensor/reset",
            .method = HTTP_POST,
            .handler = sensor_reset_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &sensor_reset_uri);

        httpd_uri_t ota_start_uri = {
            .uri = "/api/ota/start",
            .method = HTTP_POST,
            .handler = ota_start_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &ota_start_uri);

        httpd_uri_t ota_status_uri = {
            .uri = "/api/ota/status",
            .method = HTTP_GET,
            .handler = ota_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &ota_status_uri);
        
        // Register POST /api/system/reset
        httpd_uri_t reset_uri = {
            .uri = "/api/system/reset",
            .method = HTTP_POST,
            .handler = system_reset_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &reset_uri);
        
        // Captive portal detection endpoints (Apple, Android, Windows, Firefox)
        const char *captive_uris[] = {
            "/hotspot-detect.html",
            "/library/test/success.html",
            "/generate_204",
            "/gen_204",
            "/connecttest.txt",
            "/redirect",
            "/ncsi.txt",
            "/canonical.html",
            "/success.txt",
            NULL
        };
        for (int i = 0; captive_uris[i] != NULL; i++) {
            httpd_uri_t cp_uri = {
                .uri = captive_uris[i],
                .method = HTTP_GET,
                .handler = captive_redirect_handler,
                .user_ctx = NULL
            };
            httpd_register_uri_handler(server, &cp_uri);
        }
        
        ESP_LOGI(TAG, "HTTP server ready - endpoints + captive portal registered");
        
        return server;
    }
    
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return NULL;
}

// ============================================================================
// Phase 1: Main Application Entry
// ============================================================================

/**
 * @brief FreeRTOS app_main - System initialization and task creation
 */
void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "🚀 bosch-tank %s (Build #%d)", VERSION_STRING, BUILD_NUMBER);
    ESP_LOGI(TAG, "   Compiled: %s", BUILD_TIMESTAMP);
    
    // Get chip info (v6.0 API change: requires pointer argument)
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "   Hardware: ESP32 (rev %d)", chip_info.revision);
    
    ESP_LOGI(TAG, "===========================================");
    
    // Initialize hardware
    ESP_LOGI(TAG, "🔧 Initializing hardware...");
    
    ESP_LOGI(TAG, "   → Testing NVS init...");
    esp_err_t ret = init_nvs();
    ESP_LOGI(TAG, "     NVS result: %s (0x%X)", esp_err_to_name(ret), ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ NVS initialization failed!");
        return;
    }
    
    // Initialize sys_state mutex
    sys_state_mutex = xSemaphoreCreateMutex();
    if (sys_state_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create sys_state mutex!");
        return;
    }
    wifi_state_mutex = xSemaphoreCreateMutex();
    if (wifi_state_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create wifi_state mutex!");
        return;
    }
    ota_state_mutex = xSemaphoreCreateMutex();
    if (ota_state_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create ota_state mutex!");
        return;
    }
    ESP_LOGI(TAG, "   ✓ sys_state mutex created");
    
    ESP_LOGI(TAG, "   → Testing I2C init...");
    ret = init_i2c();
    ESP_LOGI(TAG, "     I2C result: %s (0x%X)", esp_err_to_name(ret), ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ I2C initialization failed!");
        return;
    }
    
    ESP_LOGI(TAG, "   → Testing GPIO init...");
    ret = init_gpio();
    ESP_LOGI(TAG, "     GPIO result: %s (0x%X)", esp_err_to_name(ret), ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ GPIO initialization failed!");
        return;
    }

    ESP_LOGI(TAG, "   → Testing touch key init...");
    ret = init_touch_key();
    ESP_LOGI(TAG, "     Touch key result: %s (0x%X)", esp_err_to_name(ret), ret);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "⚠️ Touch key unavailable - continuing without hardware touch trigger");
    }
    
    ESP_LOGI(TAG, "✅ Hardware initialized successfully");
    
    // Configure FreeRTOS Watchdog - will feed from sensor and valve tasks
    // Note: v6.0 initializes TWDT automatically, don't call init again!
    // Just reconfigure if needed
    __attribute__((unused)) esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 10000,  // 10 second timeout (10000 ms)
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,  // Monitor all cores
        .trigger_panic = true,  // Panic instead of reboot
    };
    // Don't init, just reconfigure - watchdog is auto-initialized in v6.0
    // ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_config));
    ESP_LOGI(TAG, "   → Watchdog already initialized by framework, skipping re-init");
    
    // Start FreeRTOS tasks
    ESP_LOGI(TAG, "📋 Creating FreeRTOS tasks...");
    
    BaseType_t task_result = xTaskCreatePinnedToCore(
        sensor_task,
        "sensor_task",
        TASK_STACK_SENSOR,
        NULL,
        TASK_PRIO_SENSOR,
        &sensor_task_handle,
        TASK_CORE_SAFETY
    );
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create sensor_task");
        return;
    }
    esp_task_wdt_add(sensor_task_handle);  // Register with watchdog
    ESP_LOGI(TAG, "   ✓ sensor_task (priority %d, stack %d bytes, core %d)", TASK_PRIO_SENSOR, TASK_STACK_SENSOR, TASK_CORE_SAFETY);
    
    task_result = xTaskCreatePinnedToCore(
        valve_task,
        "valve_task",
        TASK_STACK_VALVE,
        NULL,
        TASK_PRIO_VALVE,
        &valve_task_handle,
        TASK_CORE_SAFETY
    );
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create valve_task");
        return;
    }
    esp_task_wdt_add(valve_task_handle);  // Register with watchdog
    ESP_LOGI(TAG, "   ✓ valve_task (priority %d, stack %d bytes, core %d)", TASK_PRIO_VALVE, TASK_STACK_VALVE, TASK_CORE_SAFETY);

#if CONFIG_IDF_TARGET_ESP32
    if (touch_key_enabled) {
        task_result = xTaskCreatePinnedToCore(
            touch_key_task,
            "touch_task",
            TASK_STACK_TOUCH,
            NULL,
            TASK_PRIO_MAIN,
            &touch_task_handle,
            TASK_CORE_SAFETY
        );
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "❌ Failed to create touch_task");
            return;
        }
        ESP_LOGI(TAG, "   ✓ touch_task (priority %d, stack %d bytes, core %d)", TASK_PRIO_MAIN, TASK_STACK_TOUCH, TASK_CORE_SAFETY);
    }
#endif
    
    // ========== Initialize WiFi & HTTP BEFORE creating wifi_task ==========
    ESP_LOGI(TAG, "📡 Initializing WiFi...");
    
    // Initialize event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create network interfaces  
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_t *sta_netif __attribute__((unused)) = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    
    // Configure AP IP (static) - 10.1.1.1/24
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 10, 1, 1, 1);
    IP4_ADDR(&ip_info.gw, 10, 1, 1, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    
    // Set DNS to ourselves for captive portal redirect
    esp_netif_dns_info_t dns_info;
    dns_info.ip.u_addr.ip4.addr = ip_info.ip.addr;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    
    esp_netif_dhcps_start(ap_netif);
    ESP_LOGI(TAG, "   AP IP: " AP_MODE_IP_ADDR " (captive DNS active)");
    
    // Initialize WiFi driver
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    
    // Register WiFi event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    
    // Read WiFi credentials from NVS (if saved)
    char nvs_ssid[32] = {0};
    char nvs_pass[64] = {0};
    size_t ssid_len = sizeof(nvs_ssid);
    size_t pass_len = sizeof(nvs_pass);
    
    esp_err_t ret_ssid = nvs_get_str(sys_state.nvs_handle, NVS_KEY_WIFI_SSID, nvs_ssid, &ssid_len);
    esp_err_t ret_pass = nvs_get_str(sys_state.nvs_handle, NVS_KEY_WIFI_PASS, nvs_pass, &pass_len);
    
    // Use NVS credentials if available, otherwise use defaults
    const char *use_ssid = (ret_ssid == ESP_OK && ssid_len > 0) ? nvs_ssid : "ESP";
    const char *use_pass = (ret_pass == ESP_OK && pass_len > 0) ? nvs_pass : "11111111";
    
    update_wifi_credentials_state(use_ssid);
    
    ESP_LOGI(TAG, "📶 WiFi credentials - SSID: %s (from %s)", 
             use_ssid, (ret_ssid == ESP_OK) ? "NVS" : "default");
    
    // Configure WiFi STA (Station) mode
    wifi_config_t sta_config = {
        .sta = {
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strncpy((char *)sta_config.sta.ssid, use_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, use_pass, sizeof(sta_config.sta.password) - 1);
    
    // Configure AP (Access Point) for fallback/setup mode
    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, WIFI_SSID_AP_MODE, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(WIFI_SSID_AP_MODE);
    strncpy((char *)ap_config.ap.password, WIFI_PASS_AP_MODE, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = WIFI_CHANNEL_AP;
    ap_config.ap.authmode = WIFI_AUTH_AP;
    ap_config.ap.max_connection = WIFI_MAX_CONN_AP;
    ap_config.ap.beacon_interval = WIFI_BEACON_INTERVAL_AP;
    ap_config.ap.pmf_cfg.capable = true;
    ap_config.ap.pmf_cfg.required = false;
    
    // Configure AP settings while AP interface is available, then return to STA-only.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // DNS for captive portal already set above via DHCP option
    
    ESP_LOGI(TAG, "📡 WiFi configured - STA mode with fallback AP");
    ESP_LOGI(TAG, "   STA: will try SSID '%s' (3 attempts à 3 sec)", use_ssid);
    ESP_LOGI(TAG, "   AP fallback: %s @ " AP_MODE_IP_ADDR " (only after failed STA retries)", WIFI_SSID_AP_MODE);
    ESP_LOGI(TAG, "   Web server: http://<sta-ip>/ normally, http://" AP_MODE_IP_ADDR "/ in fallback");
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Optimize WiFi TX power & power saving
    esp_wifi_set_max_tx_power(84);  // 20.5 dBm
    esp_wifi_set_ps(WIFI_PS_NONE);   // Disable power saving
    
    // Initialize and start HTTP server
    httpd_handle_t server = start_webserver();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
    
    // ========== Now create wifi_task (only monitoring) ==========
    task_result = xTaskCreatePinnedToCore(
        wifi_task,
        "wifi_task",
        TASK_STACK_WIFI,
        NULL,
        TASK_PRIO_WIFI,
        &wifi_task_handle,
        TASK_CORE_NETWORK
    );
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create wifi_task");
        return;
    }
    // DO NOT register wifi_task with watchdog - it doesn't do critical work
    ESP_LOGI(TAG, "   ✓ wifi_task (priority %d, stack %d bytes, core %d - monitoring only)", TASK_PRIO_WIFI, TASK_STACK_WIFI, TASK_CORE_NETWORK);
    
    task_result = xTaskCreatePinnedToCore(
        stack_monitor_task,
        "stack_monitor_task",
        TASK_STACK_STACK_MONITOR,
        NULL,
        TASK_PRIO_MAIN,
        &stack_monitor_task_handle,
        TASK_CORE_NETWORK
    );
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create stack_monitor_task");
        return;
    }
    ESP_LOGI(TAG, "   ✓ stack_monitor_task (priority %d, stack %d bytes, core %d)", TASK_PRIO_MAIN, TASK_STACK_STACK_MONITOR, TASK_CORE_NETWORK);
    
    ESP_LOGI(TAG, "✅ All tasks created and running");
    ESP_LOGI(TAG, "📡 Configured thresholds:");
    ESP_LOGI(TAG, "   - OBEN (Tank FULL):  %d cm ← Valve closes when reached", sys_state.threshold_top);
    ESP_LOGI(TAG, "   - UNTEN (Tank EMPTY): %d cm ← Valve opens when reached", sys_state.threshold_bottom);
    ESP_LOGI(TAG, "   - Timeout (max fill): %d ms ← Safety cutoff after this duration", sys_state.timeout_max);
    
    // LED indicates system ready
    gpio_set_level(GPIO_LED_STATUS, 0);  // LED off (ready)
    
    ESP_LOGI(TAG, "🎯 System ready - waiting for sensor data...");
}


