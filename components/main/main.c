/**
 * delongi-tank: Automated Water Tank Management System
 * ESP32-based automatic filling control for coffee machines with VL53L0X ToF sensor
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
#include "driver/i2c.h"

// WiFi & Network
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"  // For IP4_ADDR macro
#include "lwip/sockets.h"   // For DNS captive portal server

// Web Server
#include "esp_http_server.h"

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

static const char *TAG = "delongi-tank-main";

#define API_VERSION "1.0"

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
    bool emergency_stop_active;
    char emergency_stop_reason[128];  // Grund für Notstopp
    uint32_t valve_open_count;  // Anzahl Ventilöffnungen
    uint32_t emergency_trigger_count;  // Anzahl Notaus-Ausloesungen
    uint32_t total_open_time_ms;  // Gesamte Öffnungszeit in ms
    float total_liters;  // Gesamte Liter basierend auf Durchfluss
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
    .emergency_stop_active = false,
    .emergency_stop_reason = "",
    .valve_open_count = 0,
    .emergency_trigger_count = 0,
    .total_open_time_ms = 0,
    .total_liters = 0.0f,
    .current_valve_open_start_ms = 0,
    .last_update_timestamp = 0
};

// Mutex for sys_state access
static SemaphoreHandle_t sys_state_mutex = NULL;

// Task Handles
static TaskHandle_t sensor_task_handle = NULL;
static TaskHandle_t valve_task_handle = NULL;
static TaskHandle_t wifi_task_handle = NULL;

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
    if (nvs_get_u32(sys_state.nvs_handle, "emergency_stop_active", &stored_emergency) == ESP_OK) {
        sys_state.emergency_stop_active = (bool)stored_emergency;
        ESP_LOGI(TAG, "Loaded emergency_stop_active from NVS: %d", sys_state.emergency_stop_active);
    }
    
    ESP_LOGI(TAG, "NVS initialized successfully");
    return ESP_OK;
}

// ============================================================================
// Phase 1: I2C Initialization (for VL53L0X sensor)
// ============================================================================

/**
 * @brief Initialize I2C bus for VL53L0X ToF sensor (ESP-IDF v6.0 API)
 */
static esp_err_t init_i2c(void)
{
    ESP_LOGI(TAG, "Initializing I2C bus (ESP-IDF v6.0)...");
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_I2C_SDA,
        .scl_io_num = GPIO_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        .clk_flags = I2C_SCLK_SRC_FLAG_FOR_NOMAL,
    };
    
    esp_err_t ret = i2c_param_config(I2C_MASTER_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = i2c_driver_install(I2C_MASTER_PORT, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "✅ I2C initialized on SDA=%d, SCL=%d", GPIO_I2C_SDA, GPIO_I2C_SCL);
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
    
    // Configure Valve control pin (GPIO 16) - OUTPUT
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

// ============================================================================
// Phase 1: HTTP Server Setup (Forward Declaration)
// ============================================================================
static httpd_handle_t start_webserver(void);

// ============================================================================
// Phase 3: HTTP REST API Handlers
// ============================================================================

/**
 * @brief Helper: Send JSON response
 */
static void send_json_response(httpd_req_t *req, const char *json_data)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_data, strlen(json_data));
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
    int percent = (int)(((float)(range - current) / (float)range) * 100.0f);

    if (percent < 0) {
        return 0;
    }

    if (percent > 100) {
        return 100;
    }

    return percent;
}

static void persist_runtime_counters(void)
{
    if (sys_state.nvs_handle == 0) {
        return;
    }

    nvs_set_u32(sys_state.nvs_handle, NVS_KEY_VALVE_OPEN_COUNT, sys_state.valve_open_count);
    nvs_set_u32(sys_state.nvs_handle, NVS_KEY_EMERGENCY_COUNT, sys_state.emergency_trigger_count);
    nvs_set_u32(sys_state.nvs_handle, NVS_KEY_TOTAL_OPEN_TIME_MS, sys_state.total_open_time_ms);
    nvs_set_u32(sys_state.nvs_handle, NVS_KEY_TOTAL_LITERS_CENTI, (uint32_t)(sys_state.total_liters * 100.0f));
    nvs_commit(sys_state.nvs_handle);
}

static void trigger_emergency_stop(const char *reason)
{
    bool was_active = sys_state.emergency_stop_active;

    sys_state.emergency_stop_active = true;
    if (reason != NULL) {
        strncpy(sys_state.emergency_stop_reason, reason, sizeof(sys_state.emergency_stop_reason) - 1);
        sys_state.emergency_stop_reason[sizeof(sys_state.emergency_stop_reason) - 1] = '\0';
    }

    if (!was_active) {
        sys_state.emergency_trigger_count++;
    }

    nvs_set_u32(sys_state.nvs_handle, "emergency_stop_active", 1);
    persist_runtime_counters();
}

static void reset_emergency_stop(void)
{
    sys_state.emergency_stop_active = false;
    strcpy(sys_state.emergency_stop_reason, "");
    nvs_set_u32(sys_state.nvs_handle, "emergency_stop_active", 0);
    nvs_commit(sys_state.nvs_handle);
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
    persist_runtime_counters();
    xSemaphoreGive(sys_state_mutex);
}

/**
 * @brief Handler: GET /api/status - Return system status as JSON
 */
static esp_err_t status_handler(httpd_req_t *req)
{
    char json_response[1024];
    int free_mem = esp_get_free_heap_size();
    system_state_t state_snapshot;

    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    state_snapshot = sys_state;
    snprintf(json_response, sizeof(json_response),
        "{"
        "\"status\":\"%s\","
        "\"emergency\":%s,"
        "\"emergency_reason\":\"%s\","
        "\"timestamp\":%lld,"
        "\"sensors\":{"
        "\"tank_level_cm\":%d,"
        "\"fill_percent\":%d,"
        "\"tank_full\":%d"
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
        "\"app_version\":\"%s\","
        "\"api_version\":\"%s\""
        "}"
        "}",
        state_snapshot.emergency_stop_active ? "EMERGENCY_STOP" : "OK",
        state_snapshot.emergency_stop_active ? "true" : "false",
        state_snapshot.emergency_stop_reason,
        (long long)(esp_timer_get_time() / 1000000),
        state_snapshot.sensor_distance_cm,
        calculate_fill_percent(state_snapshot.sensor_distance_cm, state_snapshot.threshold_top, state_snapshot.threshold_bottom),
        calculate_fill_percent(state_snapshot.sensor_distance_cm, state_snapshot.threshold_top, state_snapshot.threshold_bottom),
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
        (long long)esp_timer_get_time() / 1000,
        wifi_state.is_connected ? "true" : "false",
        VERSION_STRING,
        API_VERSION
    );
    xSemaphoreGive(sys_state_mutex);
    
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
    
    xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
    state_snapshot = sys_state;
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
    xSemaphoreGive(sys_state_mutex);
    
    send_json_response(req, json_response);
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/config - Update configuration
 */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    
    // Simple JSON parsing for thresholds
    int top = -1, bottom = -1, timeout = -1, fill_progress_timeout = -1;
    float flow_rate = -1.0f;
    
    // Parse JSON manually
    char *ptr = buf;
    while (*ptr) {
        if (strstr(ptr, "\"threshold_top_cm\":")) {
            ptr = strstr(ptr, ":") + 1;
            top = atoi(ptr);
        } else if (strstr(ptr, "\"threshold_bottom_cm\":")) {
            ptr = strstr(ptr, ":") + 1;
            bottom = atoi(ptr);
        } else if (strstr(ptr, "\"timeout_max_ms\":")) {
            ptr = strstr(ptr, ":") + 1;
            timeout = atoi(ptr);
        } else if (strstr(ptr, "\"fill_progress_timeout_ms\":")) {
            ptr = strstr(ptr, ":") + 1;
            fill_progress_timeout = atoi(ptr);
        } else if (strstr(ptr, "\"flow_rate_l_per_min\":")) {
            ptr = strstr(ptr, ":") + 1;
            flow_rate = atof(ptr);
        }
        ptr++;
    }
    
    if (top >= 0 && bottom >= 0 && timeout >= 0 && fill_progress_timeout >= 0 && flow_rate >= 0.0f) {

        if (top > 0 && bottom > top && timeout >= 1000 && fill_progress_timeout >= 1000 && flow_rate > 0.0f) {
            xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
            sys_state.threshold_top = top;
            sys_state.threshold_bottom = bottom;
            sys_state.timeout_max = timeout;
            sys_state.fill_progress_timeout_ms = fill_progress_timeout;
            sys_state.flow_rate_l_per_min = flow_rate;
            xSemaphoreGive(sys_state_mutex);
            
            // Save to NVS
            nvs_set_u32(sys_state.nvs_handle, NVS_KEY_THRESHOLD_TOP, top);
            nvs_set_u32(sys_state.nvs_handle, NVS_KEY_THRESHOLD_BOTTOM, bottom);
            nvs_set_u32(sys_state.nvs_handle, NVS_KEY_VALVE_TIMEOUT_MAX, timeout);
            nvs_set_u32(sys_state.nvs_handle, NVS_KEY_FILL_PROGRESS_TIMEOUT, fill_progress_timeout);
            nvs_set_u32(sys_state.nvs_handle, NVS_KEY_FLOW_RATE, (uint32_t)(flow_rate * 100.0f));  // Store as int * 100
            nvs_commit(sys_state.nvs_handle);
            
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
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    
    if (ret <= 0) {
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
        if (action == 1 && sys_state.emergency_stop_active) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Emergency stop active");
            return ESP_FAIL;
        }

        if (action == 1 && sys_state.sensor_distance_cm <= sys_state.threshold_top) {
            sys_state.manual_fill_active = false;

            char response[256];
            snprintf(response, sizeof(response),
                "{\"status\":\"OK\",\"action\":\"close\",\"manual_fill_active\":false,\"message\":\"Tank already at OBEN threshold\"}"
            );
            send_json_response(req, response);
            return ESP_OK;
        }

        sys_state.manual_fill_active = (action == 1);
        ESP_LOGI(TAG, "Manual fill %s requested", action ? "START" : "STOP");
        
        char response[256];
        snprintf(response, sizeof(response),
            "{\"status\":\"OK\",\"action\":\"%s\",\"manual_fill_active\":%s}", 
            action ? "open" : "close",
            sys_state.manual_fill_active ? "true" : "false");
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
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    bool do_reset = true;

    if (ret > 0 && strstr(buf, "\"action\":\"trigger\"")) {
        do_reset = false;
    }

    if (do_reset) {
        if (sys_state.emergency_stop_active) {
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
    sys_state.manual_fill_active = false;
    gpio_set_level(GPIO_VALVE_CONTROL, 0);
    sys_state.valve_state = false;
    finalize_active_valve_session(esp_timer_get_time() / 1000);
    
    char response[256];
    snprintf(response, sizeof(response),
        "{\"status\":\"%s\",\"emergency\":%s,\"message\":\"%s\",\"valve\":\"CLOSED\"}",
        sys_state.emergency_stop_active ? "EMERGENCY" : "OK",
        sys_state.emergency_stop_active ? "true" : "false",
        sys_state.emergency_stop_active ? "Emergency activated" : "Emergency reset"
    );
    send_json_response(req, response);
    
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/valve/stop - Force valve closed
 */
static esp_err_t valve_stop_handler(httpd_req_t *req)
{
    sys_state.manual_fill_active = false;
    sys_state.valve_state = false;  // Close valve
    gpio_set_level(GPIO_VALVE_CONTROL, 0);
    finalize_active_valve_session(esp_timer_get_time() / 1000);
    
    char response[128];
    snprintf(response, sizeof(response),
        "{\"status\":\"OK\",\"valve\":\"CLOSED\",\"message\":\"Valve stopped\"}"
    );
    send_json_response(req, response);
    return ESP_OK;
}

/**
 * @brief Handler: POST /api/system/reset - Soft reset (keep WiFi)
 */
static esp_err_t system_reset_handler(httpd_req_t *req)
{
    sys_state.manual_fill_active = false;
    sys_state.valve_state = false;
    gpio_set_level(GPIO_VALVE_CONTROL, 0);
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

/**
 * @brief Handler: GET /api/wifi/status - WiFi connection status
 */
static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    // Get current WiFi mode and status
    wifi_ap_record_t ap_info;
    char response[512];
    
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    
    // Get local IP if connected
    char ip_str[16] = "Not connected";
    if (err == ESP_OK) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(netif, &ip_info);
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }
    
    snprintf(response, sizeof(response),
        "{\"wifi\":"
        "{\"ssid\":\"%s\","
        "\"rssi\":%d,"
        "\"connected\":%s,"
        "\"ip\":\"%s\"}}", 
        (err == ESP_OK) ? (char*)ap_info.ssid : "Not connected",
        (err == ESP_OK) ? ap_info.rssi : 0,
        (err == ESP_OK) ? "true" : "false",
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
    int ret = httpd_req_recv(req, buffer, content_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read body");
        return ESP_FAIL;
    }
    buffer[ret] = '\0';
    
    // Extract SSID and Password using simple string parsing
    // Format: {"ssid":"xxxx","password":"yyyy"}
    char ssid[32] = {0};
    char password[64] = {0};
    
    // Parse SSID
    const char *ssid_start = strstr(buffer, "\"ssid\":\"");
    if (ssid_start) {
        ssid_start += 8;  // Skip '"ssid":"'
        const char *ssid_end = strchr(ssid_start, '"');
        if (ssid_end) {
            int ssid_len = ssid_end - ssid_start;
            if (ssid_len < 32) {
                strncpy(ssid, ssid_start, ssid_len);
            }
        }
    }
    
    // Parse Password
    const char *pass_start = strstr(buffer, "\"password\":\"");
    if (pass_start) {
        pass_start += 12;  // Skip '"password":"'
        const char *pass_end = strchr(pass_start, '"');
        if (pass_end) {
            int pass_len = pass_end - pass_start;
            if (pass_len < 64) {
                strncpy(password, pass_start, pass_len);
            }
        }
    }
    
    // Validate input
    if (strlen(ssid) == 0 || strlen(password) == 0) {
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
    nvs_commit(sys_state.nvs_handle);
    
    // Update in-memory state
    strncpy(wifi_state.ssid, ssid, sizeof(wifi_state.ssid) - 1);
    wifi_state.retry_count = 0;  // Reset retry counter
    
    // Apply new WiFi credentials immediately
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    
    // If was in AP-only fallback, ensure APSTA mode for reconnect
    if (wifi_state.ap_active) {
        ESP_LOGI(TAG, "Resetting retry count, attempting STA reconnect");
        wifi_state.ap_active = false;
    }
    
    // Disconnect any existing STA connection, then reconnect
    esp_wifi_disconnect();
    esp_wifi_connect();
    
    ESP_LOGI(TAG, "WiFi: Connecting to '%s'", ssid);
    
    char response[200];
    snprintf(response, sizeof(response),
        "{\"status\":\"OK\",\"message\":\"WiFi credentials saved. Connecting to '%s'...\"}",
        ssid
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
    uint8_t *rx_buf = malloc(512);
    uint8_t *tx_buf = malloc(512);

    if (rx_buf == NULL || tx_buf == NULL) {
        ESP_LOGE(TAG, "DNS: Failed to allocate buffers");
        free(rx_buf);
        free(tx_buf);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: Failed to create socket");
        free(rx_buf);
        free(tx_buf);
        vTaskDelete(NULL);
        return;
    }
    
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS: Failed to bind port 53");
        close(sock);
        free(rx_buf);
        free(tx_buf);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS captive portal server started on port 53");

    struct sockaddr_in client_addr;
    
    // Our AP IP in network byte order
    uint32_t ap_ip = esp_ip4addr_aton(AP_MODE_IP_ADDR);
    
    while (1) {
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&client_addr, &addr_len);
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
}

/**
 * @brief Handler: GET / - Root HTML page with modern UI
 */
static esp_err_t index_handler(httpd_req_t *req)
{
    // Lightweight HTML UI - no emojis, minimal size for reliable transfer
    static const char index_html[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DELONGI TANK</title>
<style>
body{font-family:Arial,sans-serif;background:linear-gradient(180deg,#5d76df 0%,#7ea7ff 100%);margin:0;padding:10px}
.container{max-width:460px;margin:0 auto;background:white;padding:12px;border-radius:14px;box-shadow:0 8px 24px rgba(15,23,42,0.18)}
h1{color:#1f2937;margin:0;font-size:19px}
.tabs{display:flex;gap:5px;margin:12px 0;border-bottom:2px solid #e5e7eb}
.tab-btn{flex:1;padding:9px 8px;border:none;background:#eef2f7;cursor:pointer;font-weight:bold;border-radius:8px 8px 0 0}
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
.buttons{display:flex;gap:8px;margin:10px 0 8px 0;flex-wrap:wrap}
button{flex:1;min-width:90px;padding:10px;border:none;border-radius:8px;font-weight:bold;cursor:pointer;font-size:12px}
.btn-primary{background:#667eea;color:white}
.btn-danger{background:#f44336;color:white}
.btn-success{background:#4caf50;color:white}
.btn-secondary{background:#ddd}
.btn-reset{background:#4caf50;color:white}
.btn-reset.active{background:#f44336;color:white}
.status-ok{color:#2e7d32;font-weight:bold}
.status-emergency{color:#c62828;font-weight:bold}
label{display:block;font-weight:bold;margin:8px 0 4px 0;font-size:12px;color:#333}
input{width:100%;padding:8px;margin:0 0 8px 0;box-sizing:border-box;border-radius:4px;border:1px solid #ddd;font-size:14px}
.msg{padding:10px;margin:8px 0;border-radius:4px;font-size:12px}
.error{background:#ffebee;color:#f44336}
.success{background:#e8f5e9;color:#4caf50}
@media (max-width:390px){.dashboard-hero{grid-template-columns:1fr}.container{padding:10px}.big-num{font-size:34px}}
</style></head>
<body>
<div class="container">
<h1>DELONGI TANK</h1>

<div class="tabs">
<button class="tab-btn active" onclick="switchTab(event, 'dashboard')">Dashboard</button>
<button class="tab-btn" onclick="switchTab(event, 'settings')">Settings</button>
<button class="tab-btn" onclick="switchTab(event, 'wifi')">WiFi</button>
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
<div class="pill"><span>Ventil</span><b id="valve">GESCHL</b></div>
<div class="pill"><span>WiFi</span><b id="dash-wifi">OK</b></div>
<div class="pill"><span>Notaus</span><b id="emergency-state" class="status-ok">FREI</b></div>
</div>
<div id="emergency-reason" style="display:none;padding:8px;background:#ffebee;border-radius:4px;margin:5px 0;font-size:12px;color:#c62828"></div>

<div class="buttons">
<button class="btn-primary" id="fill-btn" onclick="fill()">BEFUELLEN</button>
<button class="btn-danger" onclick="stop()">STOPP</button>
<button class="btn-reset" id="emergency-btn" onclick="resetEmergency()">RESET</button>
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

</div>

<script>
let isFilling = false;
let isEmergencyActive = false;
let dashboardTimer = null;
let dashboardPollInFlight = false;
let settingsSaveInFlight = false;
const DASHBOARD_REFRESH_MS = 1000;
function scheduleDashboardRefresh(delay){
    if(dashboardTimer) clearTimeout(dashboardTimer);
    dashboardTimer = setTimeout(() => updateDashboard(), delay);
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
            btn.textContent = isFilling ? 'BEFUELLEN STOPPEN' : 'BEFUELLEN';
            btn.disabled = isEmergencyActive;
            btn.style.opacity = isEmergencyActive ? '0.5' : '1';
        }
}
function syncEmergencyState(active){
    isEmergencyActive = !!active;
    const stateEl = document.getElementById('emergency-state');
    const btn = document.getElementById('emergency-btn');
    if(stateEl){
        stateEl.textContent = isEmergencyActive ? 'AKTIV' : 'FREI';
        stateEl.className = isEmergencyActive ? 'status-emergency' : 'status-ok';
    }
    if(btn){
        btn.className = isEmergencyActive ? 'btn-reset active' : 'btn-reset';
        btn.textContent = 'RESET';
    }
    syncFillButton();
}
function switchTab(evt, t){
  document.querySelectorAll('.tab-content').forEach(e => e.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(e => e.classList.remove('active'));
  document.getElementById(t).classList.add('active');
    if(evt && evt.target) evt.target.classList.add('active');
  if(t==='settings') loadSettings();
  if(t==='wifi') loadWiFi();
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
    fetch('/api/status').then(r => r.json()).then(d => {
    const lv = d.sensors.tank_level_cm || 0;
        const full = Math.max(0, Math.min(100, Number((d.sensors && (d.sensors.fill_percent ?? d.sensors.tank_full)) || 0)));
    document.getElementById('level').textContent = lv;
    document.getElementById('percent').textContent = full.toFixed(0) + '%';
    document.getElementById('bar-fill').style.width = full + '%';
    document.getElementById('valve').textContent = d.valve.state === 'OPEN' ? 'OFFEN' : 'GESCHL';
    document.getElementById('status').textContent = d.status;
        document.getElementById('app-version').textContent = (d.system && d.system.app_version) ? d.system.app_version : '-';
    document.getElementById('open-count').textContent = Number((d.valve && (d.valve.trigger_count ?? d.valve.open_count)) || 0).toFixed(0);
    document.getElementById('emergency-count').textContent = Number((d.valve && d.valve.emergency_trigger_count) || 0).toFixed(0);
    document.getElementById('total-liters').textContent = Number((d.valve && d.valve.total_liters) || 0).toFixed(2) + ' L';
    document.getElementById('total-time').textContent = Math.floor(Number((d.valve && d.valve.total_open_time_ms) || 0) / 1000) + ' s';
        document.getElementById('dash-wifi').textContent = (d.system && d.system.wifi_connected) ? 'Verbunden' : 'Offline';
    isFilling = !!(d.valve && d.valve.manual_fill_active);
    syncEmergencyState(!!d.emergency);
    const reasonEl = document.getElementById('emergency-reason');
    if (d.emergency_reason) {
        reasonEl.textContent = 'Grund: ' + d.emergency_reason;
        reasonEl.style.display = 'block';
    } else {
        reasonEl.style.display = 'none';
    }
    }).catch(() => {}).finally(() => {
        dashboardPollInFlight = false;
        scheduleDashboardRefresh(DASHBOARD_REFRESH_MS);
    });
}
function fill(){if(isEmergencyActive){showMsg('dashboard', 'Notaus aktiv - erst RESET ausfuehren', true); return;} const nextAction = isFilling ? 'close' : 'open'; fetch('/api/valve/manual', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: nextAction})}).then(r => r.json()).then(d => {isFilling = !!d.manual_fill_active; syncFillButton(); updateDashboard(true); if(d.message) showMsg('dashboard', d.message, false);}).catch(() => updateDashboard(true));}
function stop(){fetch('/api/valve/stop', {method: 'POST'}).then(() => {isFilling = false; updateDashboard(true); showMsg('dashboard', 'Ventil geschlossen', false);});}
function resetEmergency(){if(!isEmergencyActive){showMsg('dashboard', 'Keine Notaus-Sperre aktiv', false); return;} fetch('/api/emergency_stop', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({action: 'reset'})}).then(r => r.json()).then(d => {updateDashboard(true); document.getElementById('emergency-reason').style.display = 'none'; showMsg('dashboard', d.message || 'Reset ausgefuehrt', false);}).catch(() => showMsg('dashboard', 'Reset fehlgeschlagen', true));}
function loadSettings(){fetch('/api/config').then(r => r.json()).then(d => {document.getElementById('top').value = d.config.threshold_top_cm; document.getElementById('bottom').value = d.config.threshold_bottom_cm; document.getElementById('timeout').value = d.config.timeout_max_ms; document.getElementById('fill-progress-timeout').value = d.config.fill_progress_timeout_ms; document.getElementById('flow-rate').value = d.config.flow_rate_l_per_min;});}
function saveSettings(){const top = parseInt(document.getElementById('top').value, 10); const bottom = parseInt(document.getElementById('bottom').value, 10); const timeout = parseInt(document.getElementById('timeout').value, 10); const fillProgressTimeout = parseInt(document.getElementById('fill-progress-timeout').value, 10); const flowRate = parseFloat(document.getElementById('flow-rate').value); if(!Number.isFinite(top) || !Number.isFinite(bottom) || !Number.isFinite(timeout) || !Number.isFinite(fillProgressTimeout) || !Number.isFinite(flowRate)){showMsg('settings', 'Alle Felder muessen gueltige Zahlen enthalten', true); return;} if(top < 1 || top > 100 || bottom < 1 || bottom > 100){showMsg('settings', 'OBEN und UNTEN muessen zwischen 1 und 100 cm liegen', true); return;} if(top >= bottom){showMsg('settings', 'OBEN muss kleiner als UNTEN sein', true); return;} if(timeout < 1000 || fillProgressTimeout < 1000){showMsg('settings', 'Timeout-Werte muessen mindestens 1000 ms sein', true); return;} if(flowRate <= 0 || flowRate > 50){showMsg('settings', 'Durchfluss muss zwischen 0.1 und 50 L/min liegen', true); return;} const cfg = {threshold_top_cm: top, threshold_bottom_cm: bottom, timeout_max_ms: timeout, fill_progress_timeout_ms: fillProgressTimeout, flow_rate_l_per_min: flowRate}; settingsSaveInFlight = true; syncSaveButton(); fetch('/api/config', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(cfg)}).then(async r => {if(!r.ok) throw new Error(await r.text() || ('API error: ' + r.status)); return r.json();}).then(() => {showMsg('settings', 'Einstellungen gespeichert', false); updateDashboard(true);}).catch(e => showMsg('settings', 'Fehler: ' + e.message, true)).finally(() => {settingsSaveInFlight = false; syncSaveButton();});}
function loadWiFi(){fetch('/api/wifi/status').then(r => {if(!r.ok) throw new Error('API error: '+r.status); return r.json();}).then(d => {const c=d.wifi&&d.wifi.connected; document.getElementById('wifi-con').textContent=c?'Verbunden':'Getrennt'; document.getElementById('wifi-con').style.color=c?'#4caf50':'#f44336'; document.getElementById('wifi-ssid').textContent = (d.wifi && d.wifi.ssid) ? d.wifi.ssid : '-'; document.getElementById('wifi-rssi').textContent = (d.wifi && d.wifi.rssi) ? (d.wifi.rssi + ' dBm') : '-'; document.getElementById('wifi-ip').textContent = (d.wifi && d.wifi.ip) ? d.wifi.ip : '-';}).catch(e => {console.error('loadWiFi failed:', e); document.getElementById('wifi-con').textContent='Fehler'; showMsg('wifi', 'WiFi API Fehler', true);});}
function connectWiFi(){const s = document.getElementById('new-ssid').value; const p = document.getElementById('new-pass').value; if(!s||!p) {showMsg('wifi', 'SSID und Pass erforderlich', true); return;} fetch('/api/wifi/config', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({ssid: s, password: p})}).then(r => {if(!r.ok) throw new Error('API error: '+r.status); return r.json();}).then(d => {showMsg('wifi', 'WiFi Update gesendet', false); document.getElementById('new-ssid').value = ''; document.getElementById('new-pass').value = ''; setTimeout(loadWiFi, 2000);}).catch(e => {console.error('connectWiFi failed:', e); showMsg('wifi', 'Fehler: '+e.message, true);});}
function reset(){if(confirm('System wirklich neustarten?')) fetch('/api/system/reset', {method: 'POST'}).then(() => showMsg('wifi', 'Neustart...', false)).catch(e => showMsg('wifi', 'Fehler', true));}
syncFillButton();
syncSaveButton();
updateDashboard();
</script>
</body></html>)html";
    
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

// ============================================================================
// Phase 1: Sensor Task (VL53L0X Distance Measurement)
// ============================================================================

// ============================================================================
// VL53L0X Pololu Library Port (v1.3.0 → ESP-IDF C)
// Based on: https://github.com/pololu/vl53l0x-arduino
// ============================================================================

// --- VL53L0X Register Map (from Pololu VL53L0X.h) ---
#define SYSRANGE_START                              0x00
#define SYSTEM_SEQUENCE_CONFIG                      0x01
#define SYSTEM_INTERMEASUREMENT_PERIOD              0x04
#define SYSTEM_INTERRUPT_CONFIG_GPIO                0x0A
#define GPIO_HV_MUX_ACTIVE_HIGH                     0x84
#define SYSTEM_INTERRUPT_CLEAR                      0x0B
#define RESULT_INTERRUPT_STATUS                     0x13
#define RESULT_RANGE_STATUS                         0x14
#define CROSSTALK_COMPENSATION_PEAK_RATE_MCPS       0x20
#define I2C_SLAVE_DEVICE_ADDRESS                    0x8A
#define MSRC_CONFIG_CONTROL                         0x60
#define PRE_RANGE_CONFIG_MIN_SNR                    0x27
#define PRE_RANGE_CONFIG_VALID_PHASE_LOW            0x56
#define PRE_RANGE_CONFIG_VALID_PHASE_HIGH           0x57
#define PRE_RANGE_MIN_COUNT_RATE_RTN_LIMIT          0x64
#define FINAL_RANGE_CONFIG_MIN_SNR                  0x67
#define FINAL_RANGE_CONFIG_VALID_PHASE_LOW          0x47
#define FINAL_RANGE_CONFIG_VALID_PHASE_HIGH         0x48
#define FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT 0x44
#define PRE_RANGE_CONFIG_SIGMA_THRESH_HI            0x61
#define PRE_RANGE_CONFIG_SIGMA_THRESH_LO            0x62
#define PRE_RANGE_CONFIG_VCSEL_PERIOD               0x50
#define PRE_RANGE_CONFIG_TIMEOUT_MACROP_HI          0x51
#define PRE_RANGE_CONFIG_TIMEOUT_MACROP_LO          0x52
#define FINAL_RANGE_CONFIG_VCSEL_PERIOD             0x70
#define FINAL_RANGE_CONFIG_TIMEOUT_MACROP_HI        0x71
#define FINAL_RANGE_CONFIG_TIMEOUT_MACROP_LO        0x72
#define MSRC_CONFIG_TIMEOUT_MACROP                  0x46
#define IDENTIFICATION_MODEL_ID                     0xC0
#define IDENTIFICATION_REVISION_ID                  0xC2
#define OSC_CALIBRATE_VAL                           0xF8
#define GLOBAL_CONFIG_VCSEL_WIDTH                   0x32
#define GLOBAL_CONFIG_SPAD_ENABLES_REF_0            0xB0
#define GLOBAL_CONFIG_REF_EN_START_SELECT           0xB6
#define DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD         0x4E
#define DYNAMIC_SPAD_REF_EN_START_OFFSET            0x4F
#define ALGO_PHASECAL_LIM                           0x30
#define ALGO_PHASECAL_CONFIG_TIMEOUT                0x30
#define VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV           0x89

// Module-level state
static uint8_t vl53l0x_stop_variable = 0;

// --- I2C Low-Level Helpers ---

static esp_err_t vl53l0x_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(I2C_MASTER_PORT, VL53L0X_ADDR, data, 2, 100);
}

static esp_err_t vl53l0x_write_reg16(uint8_t reg, uint16_t value)
{
    uint8_t data[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_write_to_device(I2C_MASTER_PORT, VL53L0X_ADDR, data, 3, 100);
}

static esp_err_t __attribute__((unused)) vl53l0x_write_reg32(uint8_t reg, uint32_t value)
{
    uint8_t data[5] = {reg, (uint8_t)(value >> 24), (uint8_t)(value >> 16),
                       (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_write_to_device(I2C_MASTER_PORT, VL53L0X_ADDR, data, 5, 100);
}

static esp_err_t vl53l0x_write_multi(uint8_t reg, const uint8_t *src, uint8_t count)
{
    uint8_t buf[count + 1];
    buf[0] = reg;
    memcpy(&buf[1], src, count);
    return i2c_master_write_to_device(I2C_MASTER_PORT, VL53L0X_ADDR, buf, count + 1, 100);
}

static uint8_t vl53l0x_read_reg(uint8_t reg)
{
    uint8_t value = 0;
    i2c_master_write_read_device(I2C_MASTER_PORT, VL53L0X_ADDR, &reg, 1, &value, 1, 100);
    return value;
}

static uint16_t vl53l0x_read_reg16(uint8_t reg)
{
    uint8_t data[2] = {0};
    i2c_master_write_read_device(I2C_MASTER_PORT, VL53L0X_ADDR, &reg, 1, data, 2, 100);
    return ((uint16_t)data[0] << 8) | data[1];
}

static esp_err_t vl53l0x_read_multi(uint8_t reg, uint8_t *dst, uint8_t count)
{
    return i2c_master_write_read_device(I2C_MASTER_PORT, VL53L0X_ADDR, &reg, 1, dst, count, 100);
}

// --- I2C Bus Scan (optimized) ---

static void i2c_scan_bus(void)
{
    ESP_LOGI(TAG, "🔍 I2C bus scan (0x03-0x77)...");
    int found = 0;
    for (uint8_t addr = 0x03; addr < 0x78; addr++) {
        uint8_t reg = 0;
        esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_PORT, addr, &reg, 1, 20);
        if (ret == ESP_OK) {
            const char *name = (addr == 0x29) ? " (VL53L0X)" :
                               (addr == 0x52) ? " (EEPROM)" :
                               (addr == 0x68) ? " (MPU6050)" :
                               (addr == 0x76) ? " (BME280)" : "";
            ESP_LOGI(TAG, "   ✅ 0x%02X%s", addr, name);
            found++;
        }
    }
    ESP_LOGI(TAG, "   Scan: %d device(s) found", found);
}

// --- Pololu: getSpadInfo() ---

static bool vl53l0x_get_spad_info(uint8_t *count, bool *type_is_aperture)
{
    uint8_t tmp;
    vl53l0x_write_reg(0x80, 0x01);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x00, 0x00);
    vl53l0x_write_reg(0xFF, 0x06);
    vl53l0x_write_reg(0x83, vl53l0x_read_reg(0x83) | 0x04);
    vl53l0x_write_reg(0xFF, 0x07);
    vl53l0x_write_reg(0x81, 0x01);
    vl53l0x_write_reg(0x80, 0x01);
    vl53l0x_write_reg(0x94, 0x6b);
    vl53l0x_write_reg(0x83, 0x00);
    
    uint32_t start = esp_timer_get_time() / 1000;
    while (vl53l0x_read_reg(0x83) == 0x00) {
        if ((esp_timer_get_time() / 1000 - start) > 500) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    vl53l0x_write_reg(0x83, 0x01);
    tmp = vl53l0x_read_reg(0x92);
    *count = tmp & 0x7F;
    *type_is_aperture = (tmp >> 7) & 0x01;
    
    vl53l0x_write_reg(0x81, 0x00);
    vl53l0x_write_reg(0xFF, 0x06);
    vl53l0x_write_reg(0x83, vl53l0x_read_reg(0x83) & ~0x04);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x00, 0x01);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x80, 0x00);
    return true;
}

// --- Pololu: performSingleRefCalibration() ---

static bool vl53l0x_perform_single_ref_calibration(uint8_t vhv_init_byte)
{
    vl53l0x_write_reg(SYSRANGE_START, 0x01 | vhv_init_byte);
    
    uint32_t start = esp_timer_get_time() / 1000;
    while ((vl53l0x_read_reg(RESULT_INTERRUPT_STATUS) & 0x07) == 0) {
        if ((esp_timer_get_time() / 1000 - start) > 500) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    vl53l0x_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x01);
    vl53l0x_write_reg(SYSRANGE_START, 0x00);
    return true;
}

// --- Pololu: Full init() sequence ---

static bool vl53l0x_sensor_ready(void)
{
    uint8_t id = vl53l0x_read_reg(IDENTIFICATION_MODEL_ID);
    if (id == 0xEE) {
        ESP_LOGI(TAG, "✅ VL53L0X FOUND (Model 0x%02X, Rev 0x%02X)",
                 id, vl53l0x_read_reg(IDENTIFICATION_REVISION_ID));
        return true;
    }
    ESP_LOGE(TAG, "❌ VL53L0X not found (got 0x%02X)", id);
    return false;
}

static esp_err_t vl53l0x_init(void)
{
    ESP_LOGI(TAG, "🔧 VL53L0X Pololu full init...");
    
    // Verify model ID
    if (vl53l0x_read_reg(IDENTIFICATION_MODEL_ID) != 0xEE) {
        ESP_LOGE(TAG, "Model ID mismatch");
        return ESP_FAIL;
    }
    
    // === VL53L0X_DataInit() ===
    
    // Set I/O to 2V8 mode
    vl53l0x_write_reg(VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV,
                      vl53l0x_read_reg(VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV) | 0x01);
    
    // Set I2C standard mode
    vl53l0x_write_reg(0x88, 0x00);
    vl53l0x_write_reg(0x80, 0x01);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x00, 0x00);
    vl53l0x_stop_variable = vl53l0x_read_reg(0x91);  // Critical: device-specific value
    vl53l0x_write_reg(0x00, 0x01);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x80, 0x00);
    
    // Disable SIGNAL_RATE_MSRC and SIGNAL_RATE_PRE_RANGE limit checks
    vl53l0x_write_reg(MSRC_CONFIG_CONTROL, vl53l0x_read_reg(MSRC_CONFIG_CONTROL) | 0x12);
    
    // Set signal rate limit to 0.25 MCPS (Q9.7 format)
    vl53l0x_write_reg16(FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, (uint16_t)(0.25 * (1 << 7)));
    
    vl53l0x_write_reg(SYSTEM_SEQUENCE_CONFIG, 0xFF);
    
    ESP_LOGI(TAG, "   DataInit done, stop_variable=0x%02X", vl53l0x_stop_variable);
    
    // === VL53L0X_StaticInit() ===
    
    // Get SPAD info
    uint8_t spad_count;
    bool spad_type_is_aperture;
    if (!vl53l0x_get_spad_info(&spad_count, &spad_type_is_aperture)) {
        ESP_LOGE(TAG, "getSpadInfo failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "   SPAD: count=%d, aperture=%d", spad_count, spad_type_is_aperture);
    
    // Read reference SPAD map
    uint8_t ref_spad_map[6];
    vl53l0x_read_multi(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);
    
    // Set reference SPADs
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00);
    vl53l0x_write_reg(DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4);
    
    uint8_t first_spad_to_enable = spad_type_is_aperture ? 12 : 0;
    uint8_t spads_enabled = 0;
    for (uint8_t i = 0; i < 48; i++) {
        if (i < first_spad_to_enable || spads_enabled == spad_count) {
            ref_spad_map[i / 8] &= ~(1 << (i % 8));
        } else if ((ref_spad_map[i / 8] >> (i % 8)) & 0x1) {
            spads_enabled++;
        }
    }
    vl53l0x_write_multi(GLOBAL_CONFIG_SPAD_ENABLES_REF_0, ref_spad_map, 6);
    
    ESP_LOGI(TAG, "   SPAD calibration done (%d enabled)", spads_enabled);
    
    // === Load Tuning Settings (from vl53l0x_tuning.h) ===
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x00, 0x00);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x09, 0x00);
    vl53l0x_write_reg(0x10, 0x00);
    vl53l0x_write_reg(0x11, 0x00);
    vl53l0x_write_reg(0x24, 0x01);
    vl53l0x_write_reg(0x25, 0xFF);
    vl53l0x_write_reg(0x75, 0x00);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x4E, 0x2C);
    vl53l0x_write_reg(0x48, 0x00);
    vl53l0x_write_reg(0x30, 0x20);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x30, 0x09);
    vl53l0x_write_reg(0x54, 0x00);
    vl53l0x_write_reg(0x31, 0x04);
    vl53l0x_write_reg(0x32, 0x03);
    vl53l0x_write_reg(0x40, 0x83);
    vl53l0x_write_reg(0x46, 0x25);
    vl53l0x_write_reg(0x60, 0x00);
    vl53l0x_write_reg(0x27, 0x00);
    vl53l0x_write_reg(0x50, 0x06);
    vl53l0x_write_reg(0x51, 0x00);
    vl53l0x_write_reg(0x52, 0x96);
    vl53l0x_write_reg(0x56, 0x08);
    vl53l0x_write_reg(0x57, 0x30);
    vl53l0x_write_reg(0x61, 0x00);
    vl53l0x_write_reg(0x62, 0x00);
    vl53l0x_write_reg(0x64, 0x00);
    vl53l0x_write_reg(0x65, 0x00);
    vl53l0x_write_reg(0x66, 0xA0);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x22, 0x32);
    vl53l0x_write_reg(0x47, 0x14);
    vl53l0x_write_reg(0x49, 0xFF);
    vl53l0x_write_reg(0x4A, 0x00);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x7A, 0x0A);
    vl53l0x_write_reg(0x7B, 0x00);
    vl53l0x_write_reg(0x78, 0x21);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x23, 0x34);
    vl53l0x_write_reg(0x42, 0x00);
    vl53l0x_write_reg(0x44, 0xFF);
    vl53l0x_write_reg(0x45, 0x26);
    vl53l0x_write_reg(0x46, 0x05);
    vl53l0x_write_reg(0x40, 0x40);
    vl53l0x_write_reg(0x0E, 0x06);
    vl53l0x_write_reg(0x20, 0x1A);
    vl53l0x_write_reg(0x43, 0x40);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x34, 0x03);
    vl53l0x_write_reg(0x35, 0x44);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x31, 0x04);
    vl53l0x_write_reg(0x4B, 0x09);
    vl53l0x_write_reg(0x4C, 0x05);
    vl53l0x_write_reg(0x4D, 0x04);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x44, 0x00);
    vl53l0x_write_reg(0x45, 0x20);
    vl53l0x_write_reg(0x47, 0x08);
    vl53l0x_write_reg(0x48, 0x28);
    vl53l0x_write_reg(0x67, 0x00);
    vl53l0x_write_reg(0x70, 0x04);
    vl53l0x_write_reg(0x71, 0x01);
    vl53l0x_write_reg(0x72, 0xFE);
    vl53l0x_write_reg(0x76, 0x00);
    vl53l0x_write_reg(0x77, 0x00);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x0D, 0x01);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x80, 0x01);
    vl53l0x_write_reg(0x01, 0xF8);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x8E, 0x01);
    vl53l0x_write_reg(0x00, 0x01);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x80, 0x00);
    
    ESP_LOGI(TAG, "   Tuning settings loaded");
    
    // Set interrupt config to new sample ready
    vl53l0x_write_reg(SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04);
    vl53l0x_write_reg(GPIO_HV_MUX_ACTIVE_HIGH,
                      vl53l0x_read_reg(GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10);
    vl53l0x_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x01);
    
    // Disable MSRC and TCC by default
    vl53l0x_write_reg(SYSTEM_SEQUENCE_CONFIG, 0xE8);
    
    // === VL53L0X_PerformRefCalibration() ===
    
    // VHV calibration
    vl53l0x_write_reg(SYSTEM_SEQUENCE_CONFIG, 0x01);
    if (!vl53l0x_perform_single_ref_calibration(0x40)) {
        ESP_LOGE(TAG, "VHV calibration failed");
        return ESP_FAIL;
    }
    
    // Phase calibration
    vl53l0x_write_reg(SYSTEM_SEQUENCE_CONFIG, 0x02);
    if (!vl53l0x_perform_single_ref_calibration(0x00)) {
        ESP_LOGE(TAG, "Phase calibration failed");
        return ESP_FAIL;
    }
    
    // Restore sequence config
    vl53l0x_write_reg(SYSTEM_SEQUENCE_CONFIG, 0xE8);
    
    ESP_LOGI(TAG, "✅ VL53L0X Pololu init complete (SPAD + Tuning + RefCal)");
    return ESP_OK;
}

// --- Pololu: readRangeSingleMillimeters() ---

static uint16_t vl53l0x_read_single_mm(void)
{
    // Pololu single-shot measurement sequence using stop_variable
    vl53l0x_write_reg(0x80, 0x01);
    vl53l0x_write_reg(0xFF, 0x01);
    vl53l0x_write_reg(0x00, 0x00);
    vl53l0x_write_reg(0x91, vl53l0x_stop_variable);
    vl53l0x_write_reg(0x00, 0x01);
    vl53l0x_write_reg(0xFF, 0x00);
    vl53l0x_write_reg(0x80, 0x00);
    
    // Trigger single-shot measurement
    vl53l0x_write_reg(SYSRANGE_START, 0x01);
    
    // Wait for start bit to clear
    uint32_t start = esp_timer_get_time() / 1000;
    while (vl53l0x_read_reg(SYSRANGE_START) & 0x01) {
        if ((esp_timer_get_time() / 1000 - start) > 500) return 65535;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // Wait for result interrupt
    start = esp_timer_get_time() / 1000;
    while ((vl53l0x_read_reg(RESULT_INTERRUPT_STATUS) & 0x07) == 0) {
        if ((esp_timer_get_time() / 1000 - start) > 500) return 65535;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // Read range from RESULT_RANGE_STATUS + 10 (register 0x1E)
    uint16_t range = vl53l0x_read_reg16(RESULT_RANGE_STATUS + 10);
    
    // Clear interrupt
    vl53l0x_write_reg(SYSTEM_INTERRUPT_CLEAR, 0x01);
    
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
    
    // Try to initialize VL53L0X sensor (real hardware)
    bool sensor_available = false;
    
    ESP_LOGI(TAG, "🔧 Attempting VL53L0X sensor detection...");
    if (vl53l0x_sensor_ready()) {
        ESP_LOGI(TAG, "✅ Sensor detected! Now initializing...");
        // Attempt full initialization sequence
        esp_err_t init_result = vl53l0x_init();
        if (init_result == ESP_OK) {
            sensor_available = true;
            ESP_LOGI(TAG, "✅ VL53L0X sensor initialized and ready!");
            vTaskDelay(pdMS_TO_TICKS(100));  // Wait for first measurement
        } else {
            ESP_LOGE(TAG, "❌ Sensor detected but init FAILED (code %d) - using simulation", init_result);
        }
    } else {
        ESP_LOGE(TAG, "⚠️  VL53L0X sensor NOT DETECTED - EMERGENCY STOP");
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        trigger_emergency_stop("VL53L0X sensor not detected");
        xSemaphoreGive(sys_state_mutex);
    }
    
    ESP_LOGI(TAG, "🚀 Sensor mode: %s", sensor_available ? "REAL HARDWARE" : "EMERGENCY (no sensor)");
    
    uint16_t distance_samples[SENSOR_SAMPLES] = {0};
    int sample_idx = 0;
    uint32_t stable_counter = 0;
    
    while (1) {
        // Feed watchdog
        esp_task_wdt_reset();
        
        // Read sensor - either real or simulated
        uint16_t distance_mm = 0;
        
        if (sensor_available) {
            // Read from REAL sensor (Pololu single-shot)
            distance_mm = vl53l0x_read_single_mm();
            
            // 65535 = timeout, 0 = invalid
            if (distance_mm == 65535 || distance_mm == 0 || distance_mm > 4000) {
                static uint32_t fail_count = 0;
                fail_count++;
                if ((fail_count % 10) == 0) {
                    ESP_LOGW(TAG, "❌ Invalid reading: %d mm (%d fails)", distance_mm, fail_count);
                }
                // Use last good value from filter buffer
                if (distance_samples[0] > 0) {
                    distance_mm = distance_samples[(sample_idx + SENSOR_SAMPLES - 1) % SENSOR_SAMPLES];
                } else {
                    distance_mm = 1500;  // Fallback
                }
            }
            
            // Check if distance > 30cm (300mm), emergency stop
            if (distance_mm > 3000) {  // 30cm = 300mm, but allow some margin
                xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
                if (!sys_state.emergency_stop_active) {
                    trigger_emergency_stop("Sensor reading too high (>30cm), possible sensor failure");
                    ESP_LOGE(TAG, "🚨 EMERGENCY STOP - %s", sys_state.emergency_stop_reason);
                }
                xSemaphoreGive(sys_state_mutex);
            }
        } else {
            // No sensor - emergency already triggered
            vTaskDelay(pdMS_TO_TICKS(TASK_SENSOR_INTERVAL_MS));
            continue;
        }
        
        // Moving average filter (5-sample buffer for noise reduction)
        distance_samples[sample_idx] = distance_mm;
        sample_idx = (sample_idx + 1) % SENSOR_SAMPLES;
        
        uint32_t distance_sum = 0;
        for (int i = 0; i < SENSOR_SAMPLES; i++) {
            distance_sum += distance_samples[i];
        }
        uint16_t distance_filtered_mm = distance_sum / SENSOR_SAMPLES;
        uint16_t distance_cm = distance_filtered_mm / 10;
        
        // Update system state
        xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
        int prev_distance = sys_state.sensor_distance_cm;
        sys_state.sensor_distance_cm = distance_cm;
        sys_state.last_update_timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
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
    bool filling = false;
    bool manual_mode = false;
    uint16_t progress_reference_distance = 0;
    int last_tank_state = 0;  // 0=unknown, 1=full, 2=filling, 3=empty
    
    while (1) {
        // Feed watchdog - CRITICAL for safety
        esp_task_wdt_reset();
        
        int current_tank_state = 0;
        
        // Determine tank state based on sensor readings
        if (sys_state.sensor_distance_cm <= sys_state.threshold_top) {
            current_tank_state = 1;  // FULL
        } else if (sys_state.sensor_distance_cm >= sys_state.threshold_bottom) {
            current_tank_state = 3;  // EMPTY
        } else {
            current_tank_state = 2;  // FILLING
        }

        if (sys_state.manual_fill_active && current_tank_state == 1) {
            sys_state.manual_fill_active = false;
        }

        if (filling && !sys_state.valve_state) {
            finalize_active_valve_session(esp_timer_get_time() / 1000);
            filling = false;
            manual_mode = false;
            last_progress_time_ms = 0;
            progress_reference_distance = 0;
            ESP_LOGI(TAG, "🚰 Valve CLOSED - session finalized after external stop");
        }

        if (filling && manual_mode && !sys_state.manual_fill_active) {
            gpio_set_level(GPIO_VALVE_CONTROL, 0);
            sys_state.valve_state = false;
            finalize_active_valve_session(esp_timer_get_time() / 1000);
            filling = false;
            manual_mode = false;
            last_progress_time_ms = 0;
            progress_reference_distance = 0;
            ESP_LOGI(TAG, "🚰 Valve CLOSED - manual fill stopped");
        }

        if (sys_state.manual_fill_active && !filling && !sys_state.emergency_stop_active && current_tank_state != 1) {
            gpio_set_level(GPIO_VALVE_CONTROL, 1);
            sys_state.valve_state = true;
            filling = true;
            manual_mode = true;
            fill_start_time_ms = esp_timer_get_time() / 1000;
            last_progress_time_ms = fill_start_time_ms;
            xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
            sys_state.current_valve_open_start_ms = fill_start_time_ms;
            sys_state.valve_open_count++;
            persist_runtime_counters();
            xSemaphoreGive(sys_state_mutex);
            progress_reference_distance = sys_state.sensor_distance_cm;
            ESP_LOGI(TAG, "🚰 Valve OPENED - manual fill requested");
        }
        
        // Handle state transitions
        if (current_tank_state != last_tank_state) {
            switch (current_tank_state) {
                case 1:  // Tank FULL
                    if (filling) {
                        gpio_set_level(GPIO_VALVE_CONTROL, 0);  // Close valve
                        sys_state.valve_state = false;
                        sys_state.manual_fill_active = false;
                        finalize_active_valve_session(esp_timer_get_time() / 1000);
                        filling = false;
                        manual_mode = false;
                        last_progress_time_ms = 0;
                        progress_reference_distance = 0;
                        ESP_LOGI(TAG, "🚰 Valve CLOSED - Tank is FULL (reached OBEN threshold)");
                    }
                    break;
                    
                case 3:  // Tank EMPTY
                    break;
            }
            last_tank_state = current_tank_state;
        }

        if (current_tank_state == 3 && !filling && !sys_state.emergency_stop_active && !sys_state.manual_fill_active) {
            gpio_set_level(GPIO_VALVE_CONTROL, 1);  // Open valve
            sys_state.valve_state = true;
            filling = true;
            manual_mode = false;
            fill_start_time_ms = esp_timer_get_time() / 1000;
            last_progress_time_ms = fill_start_time_ms;
            xSemaphoreTake(sys_state_mutex, portMAX_DELAY);
            sys_state.current_valve_open_start_ms = fill_start_time_ms;
            sys_state.valve_open_count++;
            persist_runtime_counters();
            xSemaphoreGive(sys_state_mutex);
            progress_reference_distance = sys_state.sensor_distance_cm;
            ESP_LOGI(TAG, "🚰 Valve OPENED - Tank is EMPTY (below UNTEN threshold) - FILLING STARTED");
        }
        
        // Timeout protection: if filling exceeds max timeout
        if (filling) {
            uint64_t now_ms = esp_timer_get_time() / 1000;
            uint16_t current_distance = sys_state.sensor_distance_cm;

            if (current_distance + FILL_PROGRESS_MIN_DELTA_CM <= progress_reference_distance) {
                progress_reference_distance = current_distance;
                last_progress_time_ms = now_ms;
            } else if (!manual_mode && (now_ms - last_progress_time_ms) > sys_state.fill_progress_timeout_ms) {
                gpio_set_level(GPIO_VALVE_CONTROL, 0);
                sys_state.valve_state = false;
                sys_state.manual_fill_active = false;
                trigger_emergency_stop("No fill progress: distance did not decrease sufficiently within timeout");
                finalize_active_valve_session(now_ms);
                filling = false;
                manual_mode = false;
                last_progress_time_ms = 0;
                progress_reference_distance = 0;
                ESP_LOGE(TAG, "🚨 EMERGENCY STOP - %s", sys_state.emergency_stop_reason);
                last_tank_state = current_tank_state;
                vTaskDelay(pdMS_TO_TICKS(TASK_VALVE_CHECK_MS));
                continue;
            }

            uint64_t elapsed_ms = now_ms - fill_start_time_ms;
            if (elapsed_ms > sys_state.timeout_max) {
                gpio_set_level(GPIO_VALVE_CONTROL, 0);  // Close valve
                sys_state.valve_state = false;
                sys_state.manual_fill_active = false;
                finalize_active_valve_session(now_ms);
                filling = false;
                manual_mode = false;
                last_progress_time_ms = 0;
                progress_reference_distance = 0;
                ESP_LOGW(TAG, "⚠️  TIMEOUT! Valve CLOSED - fill time exceeded %d ms", 
                        sys_state.timeout_max);
                // Note: Not calling emergency stop, just safety closure
            }
        }
        
        // LED feedback: ON while opening, OFF when closed
        if (sys_state.emergency_stop_active) {
            gpio_set_level(GPIO_LED_STATUS, 1);  // Red alert
        } else {
            gpio_set_level(GPIO_LED_STATUS, sys_state.valve_state ? 1 : 0);
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
        
        wifi_state.retry_count++;
        wifi_state.last_error_code = reason;
        wifi_state.last_attempt_tick = esp_timer_get_time() / 1000;  // Record timestamp
        
        // Don't retry here - let wifi_task handle retry logic
        // This prevents blocking the event loop
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✅ WiFi STA connected!");
        ESP_LOGI(TAG, "   IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        wifi_state.is_connected = true;
        wifi_state.retry_count = 0;  // Reset retry counter
        wifi_state.ap_active = false;
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
        
        // If already connected, just monitor
        if (wifi_state.is_connected) {
            vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
            continue;
        }
        
        // Not connected - check if we should retry or switch to AP mode
        if (wifi_state.ap_active) {
            // Already in AP mode - wait for user to configure WiFi
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        // In STA mode but not connected
        if (wifi_state.retry_count < 3) {
            // Check if 3 seconds have passed since last attempt
            uint32_t time_since_attempt = now - wifi_state.last_attempt_tick;
            
            if (time_since_attempt >= 3000 || wifi_state.last_attempt_tick == 0) {
                // Time to retry (or first attempt)
                wifi_state.last_attempt_tick = now;
                ESP_LOGI(TAG, "🔄 WiFi connect attempt %d/3...", wifi_state.retry_count + 1);
                esp_wifi_connect();
            }
        } else {
            // Max retries reached - AP is already running in APSTA mode
            ESP_LOGE(TAG, "❌ WiFi STA failed after 3 attempts - AP still active @ " AP_MODE_IP_ADDR);
            wifi_state.ap_active = true;
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
    ESP_LOGI(TAG, "🚀 delongi-tank %s (Build #%d)", VERSION_STRING, BUILD_NUMBER);
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
    
    strncpy(wifi_state.ssid, use_ssid, sizeof(wifi_state.ssid) - 1);
    
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
    
    // APSTA mode: STA tries to connect, AP always available for setup
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    
    // DNS for captive portal already set above via DHCP option
    
    ESP_LOGI(TAG, "📡 WiFi configured - APSTA mode");
    ESP_LOGI(TAG, "   STA: will try SSID '%s' (3 attempts à 3 sec)", use_ssid);
    ESP_LOGI(TAG, "   AP: %s @ " AP_MODE_IP_ADDR, WIFI_SSID_AP_MODE);
    ESP_LOGI(TAG, "   Web server: http://" AP_MODE_IP_ADDR "/ (AP) or http://<sta-ip>/ (STA)");
    
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
    
    // Start DNS captive portal server (resolves all domains to our AP IP)
    task_result = xTaskCreatePinnedToCore(dns_server_task, "dns_task", 8192, NULL, TASK_PRIO_WIFI, NULL, TASK_CORE_NETWORK);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create dns_task");
        return;
    }
    
    ESP_LOGI(TAG, "✅ All tasks created and running");
    ESP_LOGI(TAG, "📡 Configured thresholds:");
    ESP_LOGI(TAG, "   - OBEN (Tank FULL):  %d cm ← Valve closes when reached", sys_state.threshold_top);
    ESP_LOGI(TAG, "   - UNTEN (Tank EMPTY): %d cm ← Valve opens when reached", sys_state.threshold_bottom);
    ESP_LOGI(TAG, "   - Timeout (max fill): %d ms ← Safety cutoff after this duration", sys_state.timeout_max);
    
    // LED indicates system ready
    gpio_set_level(GPIO_LED_STATUS, 0);  // LED off (ready)
    
    ESP_LOGI(TAG, "🎯 System ready - waiting for sensor data...");
}

