/**
 * delonghi-tank: WiFi & Network Initialization
 * 
 * Features:
 * - STA mode with SSID/Password from NVS
 * - Automatic AP fallback (10.1.1.1) if STA connection fails
 * - mDNS hostname (delonghi-tank.local)
 * - SNTP for time synchronization
 */

#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_sntp.h>
#include <mdns.h>
#include <nvs_flash.h>
#include <string.h>

#include "config.h"

#define TAG "WIFI_NET"

// WiFi configuration
#define WIFI_SSID_DEFAULT WIFI_SSID_AP_MODE
#define WIFI_PASS_DEFAULT WIFI_PASS_AP_MODE
#define WIFI_AP_IP AP_MODE_IP_ADDR
#define WIFI_AP_GATEWAY AP_MODE_IP_ADDR
#define WIFI_AP_NETMASK "255.255.255.0"

static bool sta_connected = false;
static esp_netif_t *netif_sta = NULL;
static esp_netif_t *netif_ap = NULL;

// ============================================================================
// WiFi Event Handler
// ============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA starting, attempting to connect...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        sta_connected = false;
        ESP_LOGW(TAG, "STA disconnected - retrying...");
        // Retry connection (up to 15 times with backoff)
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        sta_connected = true;
        ESP_LOGI(TAG, "STA connected!");
        ESP_LOGI(TAG, "Connected IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Start mDNS only after IP acquired
        mdns_init();
        mdns_hostname_set("delonghi-tank");
        mdns_instance_name_set("delonghi-tank Water Tank Controller");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "STA [" MACSTR "] joined AP", MAC2STR(event->mac));
    }
}

// ============================================================================
// Initialize NetIF and WiFi Stack
// ============================================================================

static esp_err_t init_netif_wifi(void)
{
    // Create event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Initialize network interfaces
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create STA interface
    netif_sta = esp_netif_create_default_wifi_sta();
    
    // Create AP interface
    netif_ap = esp_netif_create_default_wifi_ap();
    
    // Configure AP IP (static)
    esp_netif_ip_info_t ip_info = {
        .ip = {.addr = ipaddr_addr(WIFI_AP_IP)},
        .gw = {.addr = ipaddr_addr(WIFI_AP_GATEWAY)},
        .netmask = {.addr = ipaddr_addr(WIFI_AP_NETMASK)}
    };
    
    esp_netif_dhcps_stop(netif_ap);
    esp_netif_set_ip_info(netif_ap, &ip_info);
    esp_netif_dhcps_start(netif_ap);
    
    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    
    return ESP_OK;
}

// ============================================================================
// Read WiFi Credentials from NVS
// ============================================================================

static esp_err_t nvs_read_wifi_credentials(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "WiFi config not found in NVS - using defaults");
        strcpy(ssid, WIFI_SSID_DEFAULT);
        strcpy(password, WIFI_PASS_DEFAULT);
        return ESP_OK;
    }
    else if (err != ESP_OK) {
        return err;
    }
    
    size_t ssid_len = 32, pass_len = 64;
    err = nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, "password", password, &pass_len);
    }
    
    nvs_close(nvs_handle);
    return err;
}

// ============================================================================
// Start WiFi in STA+AP Mode
// ============================================================================

static esp_err_t start_wifi_sta_ap(void)
{
    char ssid[32];
    char password[64];
    
    // Read credentials
    if (nvs_read_wifi_credentials(ssid, password) != ESP_OK) {
        strcpy(ssid, WIFI_SSID_DEFAULT);
        strcpy(password, WIFI_PASS_DEFAULT);
    }
    
    ESP_LOGI(TAG, "WiFi STA SSID: %s", ssid);
    
    // Configure STA mode
    wifi_config_t sta_config = {
        .sta = {
            .password = {0},
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .sae_h2e_identifier = {0},
        },
    };
    
    memcpy(sta_config.sta.ssid, (uint8_t *)ssid, strlen(ssid));
    memcpy(sta_config.sta.password, (uint8_t *)password, strlen(password));
    
    // Configure AP mode (always on for fallback)
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(WIFI_SSID_DEFAULT),
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
        },
    };
    
    memcpy(ap_config.ap.ssid, (uint8_t *)WIFI_SSID_DEFAULT, strlen(WIFI_SSID_DEFAULT));
    memcpy(ap_config.ap.password, (uint8_t *)WIFI_PASS_DEFAULT, strlen(WIFI_PASS_DEFAULT));
    
    // Set WiFi mode to STA + AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    
    ESP_LOGI(TAG, "WiFi configured in STA+AP mode");
    ESP_LOGI(TAG, "AP SSID: %s, Password: %s, IP: %s", 
             WIFI_SSID_DEFAULT, WIFI_PASS_DEFAULT, WIFI_AP_IP);
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    return ESP_OK;
}

// ============================================================================
// Initialize SNTP for Time Sync
// ============================================================================

static time_t sntp_get_current_time(void)
{
    time_t now = time(NULL);
    struct tm timeinfo = *localtime(&now);
    
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGW(TAG, "Time not set yet. Waiting for SNTP to update time...");
        return 0;
    }
    
    return now;
}

static void sntp_sync_time(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");
    
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    
    // Wait for time sync (max 15 seconds)
    int retry = 0;
    time_t now = sntp_get_current_time();
    while (now < 24 * 3600 && ++retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        now = sntp_get_current_time();
    }
    
    if (now > 24 * 3600) {
        time_t now = time(NULL);
        ESP_LOGI(TAG, "SNTP sync successful - Time set to: %s", ctime(&now));
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout - Using default time");
    }
}

// ============================================================================
// Main WiFi Init Function (PUBLIC API)
// ============================================================================

/**
 * @brief Initialize WiFi in STA+AP mode with mDNS and SNTP
 * @return ESP_OK on success
 * 
 * Operation:
 * 1. Initialize network interfaces (STA and AP)
 * 2. Start WiFi with STA config from NVS (fallback to AP if STA fails)
 * 3. Initialize mDNS (delonghi-tank.local)
 * 4. Sync time via SNTP
 */
esp_err_t init_wifi(void)
{
    ESP_LOGI(TAG, "=== WiFi & Network Initialization ===");
    
    // Initialize netif and WiFi drivers
    if (init_netif_wifi() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize netif/WiFi drivers");
        return ESP_FAIL;
    }
    
    // Start WiFi (STA + AP)
    if (start_wifi_sta_ap() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return ESP_FAIL;
    }
    
    // Sync time (after WiFi is running)
    vTaskDelay(pdMS_TO_TICKS(2000));  // Give WiFi time to connect
    sntp_sync_time();
    
    ESP_LOGI(TAG, "WiFi initialization complete");
    ESP_LOGI(TAG, "Connect via:");
    ESP_LOGI(TAG, "  - AP:   %s / %s @ %s", WIFI_SSID_DEFAULT, WIFI_PASS_DEFAULT, WIFI_AP_IP);
    ESP_LOGI(TAG, "  - mDNS: delonghi-tank.local (after STA connection)");
    
    return ESP_OK;
}

// ============================================================================
// Helper: Get WiFi Status String
// ============================================================================

const char *get_wifi_status_string(void)
{
    if (sta_connected) {
        return "STA Connected";
    } else {
        return "AP Mode (waiting for STA connection)";
    }
}
