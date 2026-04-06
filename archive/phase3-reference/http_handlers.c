/**
 * delonghi-tank: HTTP REST API Handlers
 * 
 * This file contains all HTTP endpoint handlers for the REST API.
 * Compile with: -I$(IDF_PATH)/components/esp_http_server/include
 */

#include <esp_http_server.h>
#include <json.h>

#define TAG "HTTP_API"

// ============================================================================
// Helper: JSON Response Builder
// ============================================================================

static void send_json_response(httpd_req_t *req, const char *json_data, int status_code)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status_code == 200 ? "200 OK" : status_code == 201 ? "201 Created" : "400 Bad Request");
    httpd_resp_send(req, json_data, strlen(json_data));
}

static void send_error_response(httpd_req_t *req, const char *error_msg)
{
    char json_buf[256];
    snprintf(json_buf, sizeof(json_buf), 
             "{\"error\":\"%s\",\"timestamp\":%lld}", 
             error_msg, esp_timer_get_time() / 1000000);
    send_json_response(req, json_buf, 400);
}

// ============================================================================
// Endpoint 1: GET /api/status
// Returns: Current system state (sensor values, valve status, WiFi info)
// ============================================================================

static esp_err_t status_handler(httpd_req_t *req)
{
    char json_response[512];
    int free_mem = esp_get_free_heap_size();
    
    snprintf(json_response, sizeof(json_response),
        "{"
        "\"status\":\"OK\","
        "\"timestamp\":%lld,"
        "\"device_id\":\"%s\","
        "\"version\":\"%s\","
        "\"sensors\":{"
        "  \"tank_level_cm\":%d,"
        "  \"tank_full\":%.0f%%"
        "},"
        "\"valve\":{"
        "  \"state\":\"%s\","
        "  \"mode\":\"%s\""
        "},"
        "\"system\":{"
        "  \"uptime_seconds\":%lld,"
        "  \"free_heap_bytes\":%d,"
        "  \"temperature_celsius\":%.1f"
        "}"
        "}",
        esp_timer_get_time() / 1000000,
        CONFIG_DEVICE_NAME,
        VERSION_STRING,
        sys_state.current_level_cm,
        (sys_state.current_level_cm / (float)sys_state.threshold_top) * 100.0f,
        sys_state.valve_state == 1 ? "OPEN" : "CLOSED",
        sys_state.valve_mode == 0 ? "AUTO" : "MANUAL",
        esp_timer_get_time() / 1000000000,
        free_mem,
        (float)esp_temp_sensor_read_raw() / 10.0f
    );
    
    send_json_response(req, json_response, 200);
    return ESP_OK;
}

// ============================================================================
// Endpoint 2: GET /api/config
// Returns: Current system configuration (thresholds, timeouts, WiFi SSID)
// ============================================================================

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char json_response[512];
    uint8_t ssid[32];
    wifi_mode_t mode;
    
    esp_wifi_get_mode(&mode);
    // Note: Get SSID from WiFi config
    
    snprintf(json_response, sizeof(json_response),
        "{"
        "\"config\":{"
        "  \"threshold_top_cm\":%d,"
        "  \"threshold_bottom_cm\":%d,"
        "  \"timeout_max_ms\":%d,"
        "  \"mode_auto\":true,"
        "  \"wifi\":{"
        "    \"ssid\":\"%s\","
        "    \"mode\":\"%s\""
        "  }"
        "}"
        "}",
        sys_state.threshold_top,
        sys_state.threshold_bottom,
        sys_state.timeout_max,
        "delonghi-tank",
        mode == WIFI_MODE_AP ? "AP" : "STA"
    );
    
    send_json_response(req, json_response, 200);
    return ESP_OK;
}

// ============================================================================
// Endpoint 3: POST /api/config
// Sets: Thresholds and timeout values
// Payload: {"threshold_top": 200, "threshold_bottom": 50, "timeout_max": 30000}
// ============================================================================

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            send_error_response(req, "Request timeout");
        }
        return ESP_FAIL;
    }
    
    // Simple JSON parsing (use cJSON for production)
    int top = 0, bottom = 0, timeout = 0;
    if (sscanf(buf, "{\"threshold_top\":%d,\"threshold_bottom\":%d,\"timeout_max\":%d}",
               &top, &bottom, &timeout) == 3) {
        
        if (top > 0 && bottom > 0 && timeout > 0) {
            sys_state.threshold_top = top;
            sys_state.threshold_bottom = bottom;
            sys_state.timeout_max = timeout;
            
            // Save to NVS
            // TODO: Implement nvs_set_u32(nvs_config_handle, "threshold_top", top);
            
            char response[256];
            snprintf(response, sizeof(response),
                "{\"status\":\"OK\",\"message\":\"Config updated\",\"config\":{\"threshold_top\":%d,\"threshold_bottom\":%d}}",
                top, bottom);
            send_json_response(req, response, 200);
            return ESP_OK;
        }
    }
    
    send_error_response(req, "Invalid JSON or values");
    return ESP_FAIL;
}

// ============================================================================
// Endpoint 4: POST /api/valve/manual
// Controls: Manual valve operation (OPEN/CLOSE)
// Payload: {"action": "open"} or {"action": "close"}
// ============================================================================

static esp_err_t valve_manual_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    
    if (ret <= 0) {
        send_error_response(req, "Request timeout");
        return ESP_FAIL;
    }
    
    // Parse action (simple string search)
    int action = -1;
    if (strstr(buf, "\"action\":\"open\"")) {
        action = 1;
    } else if (strstr(buf, "\"action\":\"close\"")) {
        action = 0;
    }
    
    if (action >= 0) {
        sys_state.valve_mode = 1;  // Switch to manual mode
        sys_state.valve_state = action;
        
        // Control valve GPIO
        if (action == 1) {
            gpio_set_level(GPIO_VALVE_CTRL, 1);
            ESP_LOGI(TAG, "Valve opened manually");
        } else {
            gpio_set_level(GPIO_VALVE_CTRL, 0);
            ESP_LOGI(TAG, "Valve closed manually");
        }
        
        char response[256];
        snprintf(response, sizeof(response),
            "{\"status\":\"OK\",\"action\":\"%s\",\"mode\":\"manual\"}", 
            action ? "open" : "close");
        send_json_response(req, response, 200);
        return ESP_OK;
    }
    
    send_error_response(req, "Invalid action");
    return ESP_FAIL;
}

// ============================================================================
// Endpoint 5: POST /api/emergency_stop
// Stops: All operations and closes valve
// Payload: None or {"reason": "manual_stop"}
// ============================================================================

static esp_err_t emergency_stop_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "EMERGENCY STOP triggered via API");
    
    // Stop all operations
    sys_state.valve_state = 0;      // Close valve
    sys_state.valve_mode = 1;       // Switch to manual
    gpio_set_level(GPIO_VALVE_CTRL, 0);
    gpio_set_level(GPIO_LED_ERROR, 1);  // Red LED on
    
    // Log event
    // TODO: Implement event logging
    
    char response[256];
    snprintf(response, sizeof(response),
        "{\"status\":\"EMERGENCY_STOP\",\"message\":\"All operations stopped\",\"valve\":\"CLOSED\"}");
    send_json_response(req, response, 200);
    
    return ESP_OK;
}

// ============================================================================
// Endpoint 6: GET /
// Returns: HTML web interface (embedded)
// ============================================================================

// Embedded HTML-UI (minified - ~2KB)
const char index_html[] = ""
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width'>"
    "<title>delonghi-tank</title>"
    "<style>"
    "body{font-family:Arial;background:#f0f0f0;margin:0;padding:10px}"
    ".container{max-width:800px;margin:0 auto;background:#fff;padding:20px;border-radius:8px}"
    ".status{padding:15px;background:#e8f5e9;border-left:4px solid #4caf50;margin:10px 0}"
    ".error{padding:15px;background:#ffebee;border-left:4px solid #f44336;margin:10px 0}"
    ".controls{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:20px 0}"
    "button{padding:10px 20px;border:none;border-radius:4px;cursor:pointer;font-weight:bold}"
    ".btn-open{background:#4caf50;color:white}"
    ".btn-close{background:#f44336;color:white}"
    ".btn-open:hover{background:#45a049}"
    ".btn-close:hover{background:#da190b}"
    ".gauge{width:100%;height:200px;border:2px solid #ccc;border-radius:8px;margin:20px 0;background:linear-gradient(to top,#f44336,#ffeb3b,#4caf50)}"
    "</style>"
    "</head><body>"
    "<div class='container'>"
    "<h1>🚰 delonghi-tank</h1>"
    "<div class='status'>"
    "<h3>Tank-Status</h3>"
    "<p>Pegelstand: <span id='level'>--</span> cm</p>"
    "<p>Auslastung: <span id='percent'>--</span>%</p>"
    "</div>"
    "<div class='controls'>"
    "<button class='btn-open' onclick='openValve()'>Öffnen</button>"
    "<button class='btn-close' onclick='closeValve()'>Schließen</button>"
    "</div>"
    "<div id='error' class='error' style='display:none'></div>"
    "<script>"
    "function updateStatus(){"
    "fetch('/api/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('level').textContent=d.sensors.tank_level_cm;"
    "document.getElementById('percent').textContent=Math.round(d.sensors.tank_full);"
    "}).catch(()=>{});"
    "}"
    "function openValve(){"
    "fetch('/api/valve/manual',{method:'POST',body:JSON.stringify({action:'open'})}).catch(e=>showError(e.message));"
    "}"
    "function closeValve(){"
    "fetch('/api/valve/manual',{method:'POST',body:JSON.stringify({action:'close'})}).catch(e=>showError(e.message));"
    "}"
    "function showError(msg){"
    "let e=document.getElementById('error');e.textContent=msg;e.style.display='block';"
    "}"
    "setInterval(updateStatus,2000);updateStatus();"
    "</script>"
    "</div></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

// ============================================================================
// Server Configuration
// ============================================================================

#define SERVER_PORT 80
#define SERVER_STACK_SIZE 4096
#define SERVER_TASK_PRIORITY 4
#define MAX_OPEN_SOCKETS 7
