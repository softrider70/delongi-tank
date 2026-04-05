#ifndef CONFIG_H
#define CONFIG_H

#include "sdkconfig.h"
#include <stdint.h>

/**
 * delongi-tank Configuration Header
 * 
 * Wassertank-Managementsystem für Kaffeemaschinen
 * Hardware: ESP32-DEVKITC-V4 mit VL53L0X ToF-Sensor + 12V Solenoid Ventil
 */

// ============================================================================
// GPIO Pin Configuration (ESP32-DEVKITC-V4, 38-pin)
// ============================================================================

#define GPIO_LED_STATUS     2       // Onboard LED (Status-Anzeige)
#define GPIO_I2C_SDA        21      // VL53L0X Sensor (Data)
#define GPIO_I2C_SCL        22      // VL53L0X Sensor (Clock)
#define GPIO_VALVE_CONTROL  16      // MOSFET-Modul für 12V Magnetventil
#define GPIO_TOUCH_KEY      33      // Touch-Key T8 fuer manuelles Befuellen (nur ESP32 classic)

// ============================================================================
// I2C Configuration (VL53L0X)
// ============================================================================

#define I2C_MASTER_PORT     I2C_NUM_0           // I2C Port 0
#define I2C_MASTER_FREQ_HZ  100000              // 100kHz standard mode
#define I2C_MASTER_SDA_IO   GPIO_I2C_SDA        // GPIO 21
#define I2C_MASTER_SCL_IO   GPIO_I2C_SCL        // GPIO 22
#define I2C_MASTER_RX_BUF   0
#define I2C_MASTER_TX_BUF   0
#define I2C_TIMEOUT_MS      10000               // 10s timeout

#define VL53L0X_ADDR        0x29                // Standard I2C address

// ============================================================================
// Sensor Configuration (Distance Measurement)
// ============================================================================

// Grenzwerte für Tankfüllstand (in Zentimetern)
// Sensor sitzt ÜBER dem Tank und misst Abstand zur Wasseroberfläche
// Logik: Je KLEINER der Abstand, desto VOLLER der Tank

#define TANK_THRESHOLD_TOP_DEFAULT      1       // Abstand wenn Tank VOLL (OBEN-Grenzwert, cm)
#define TANK_THRESHOLD_BOTTOM_DEFAULT   50      // Abstand wenn Tank LEER (UNTEN-Grenzwert, cm)

// Sensor-Messzyklus
#define SENSOR_READ_INTERVAL_MS         250     // 250ms = 4 Messungen/Sekunde
#define SENSOR_SAMPLES                  3       // Schnellere Mittelung fuer schnelleren Sicherheitsstopp
#define TASK_SENSOR_INTERVAL_MS         250     // Task delay für sensor_task

// Touch-Key fuer manuelles Befuellen
#define TOUCH_KEY_SAMPLE_MS             60      // Polling fuer Touch-Erkennung
#define TOUCH_KEY_FILTER_PERIOD_MS      10      // IIR Filter-Zyklus
#define TOUCH_KEY_CALIBRATION_SAMPLES   12      // Messungen fuer Start-Baseline
#define TOUCH_KEY_THRESHOLD_PERCENT     75      // Touch erkannt unter 75% der Basislinie
#define TOUCH_KEY_DEBOUNCE_COUNT        2       // Touch muss 2 Samples stabil sein
#define TOUCH_KEY_RELEASE_COUNT         2       // Release muss 2 Samples stabil sein

// ============================================================================
// Valve Control Configuration (Solenoid, via MOSFET)
// ============================================================================

// Magnetventil-Timeout beim Befüllen
#define VALVE_TIMEOUT_MAX_DEFAULT       60000   // 60 Sekunden max. Befüllung
#define VALVE_TIMEOUT_CHECK_MS          1000    // Alle 1s prüfen
#define TASK_VALVE_CHECK_MS             200     // Schnelle Reaktion fuer Sicherheitsabschaltung

// Füllfortschritt: Während geöffnetem Ventil muss der Sensorabstand kleiner werden
#define FILL_PROGRESS_MIN_DELTA_CM      1       // Mindestens 1 cm Verringerung
#define FILL_PROGRESS_TIMEOUT_DEFAULT   5000    // Innerhalb von 5s muss Fortschritt sichtbar sein
#define FILL_PROGRESS_CONFIRM_SAMPLES   3       // Fortschritt erst nach mehreren stabilen Samples bestaetigen

// Ventil-PWM oder Digital
#define VALVE_USE_PWM                   0       // 0: Digital (on/off), 1: PWM
#define VALVE_PWM_FREQ                  1000    // Nur falls PWM: 1kHz
#define VALVE_PWM_DUTY                  100     // 100% duty = voll offen

// ============================================================================
// NVS (Non-Volatile Storage) Configuration
// ============================================================================

#define NVS_NAMESPACE                   "delongi-tank"

// NVS Keys für Persistierung
#define NVS_KEY_WIFI_SSID               "wifi_ssid"
#define NVS_KEY_WIFI_PASS               "wifi_pass_enc"  // AES-encrypted
#define NVS_KEY_THRESHOLD_TOP           "thresh_top"     // Tank VOLL
#define NVS_KEY_THRESHOLD_BOTTOM        "thresh_bottom"  // Tank LEER
#define NVS_KEY_VALVE_TIMEOUT_MAX       "timeout_max"
#define NVS_KEY_FILL_PROGRESS_TIMEOUT   "fill_prog_to"
#define NVS_KEY_FLOW_RATE               "flow_rate_lpm"
#define NVS_KEY_VALVE_OPEN_COUNT        "valve_opens"
#define NVS_KEY_EMERGENCY_COUNT         "emerg_count"
#define NVS_KEY_TOTAL_OPEN_TIME_MS      "open_time_ms"
#define NVS_KEY_TOTAL_LITERS_CENTI      "tot_liters_cl"
#define NVS_KEY_EMERGENCY_STOP          "emerg_stop"
#define NVS_KEY_LAST_FULL_TIMESTAMP     "last_full"
#define NVS_KEY_ERROR_LOG               "error_log"

// NVS String-Längen
#define NVS_SSID_MAX_LEN                32
#define NVS_PASS_MAX_LEN                64
#define NVS_ERROR_LOG_MAX_LEN           256

// ============================================================================
// WiFi Configuration
// ============================================================================

#define WIFI_SSID_AP_MODE               "delongi-tank-setup"    // AP-Modus SSID
#define WIFI_PASS_AP_MODE               "password"              // AP-Modus Password (simple, ASCII)
#define WIFI_CHANNEL_AP                 6                        // Less congestion than channel 1
#define WIFI_MAX_CONN_AP                8                        // Increased from 4 to 8
#define WIFI_AUTH_AP                    WIFI_AUTH_WPA2_PSK
#define WIFI_BEACON_INTERVAL_AP         100                      // 100ms beacon interval (stable AP broadcast)

// WiFi-Verbindungs-Timeout
#define WIFI_CONNECT_TIMEOUT_MS         10000   // 10 Sekunden
#define WIFI_RETRY_MAX                  3

// === AP-Mode IP Configuration ===
// Landing-Page wird unter 10.1.1.1 erreichbar sein
#define AP_MODE_IP_ADDR                 "10.1.1.1"
#define AP_MODE_GATEWAY                 "10.1.1.1"
#define AP_MODE_NETMASK                 "255.255.255.0"
#define AP_MODE_DNS_PRIMARY             "10.1.1.1"
#define AP_MODE_DNS_SECONDARY           "8.8.8.8"

// === mDNS & Hostname Configuration ===
// mDNS ermöglicht Zugriff via "delongi-tank.local"
// Hostname wird im WiFi-Router angezeigt
#define MDNS_HOSTNAME                   "delongi-tank"          // z.B. delongi-tank.local
#define MDNS_INSTANCE_NAME              "Delongi Tank System"
#define MDNS_HTTP_SERVICE               "_http"
#define MDNS_TCP_PROTOCOL               "_tcp"
#define MDNS_PORT                       80

// ============================================================================
// HTTP Server Configuration
// ============================================================================

#define SERVER_PORT                     80              // HTTP port (bind to 0.0.0.0:80)
#define SERVER_STACK_SIZE               8192            // Increased for stability
#define SERVER_TASK_PRIORITY            9               // FreeRTOS priority
#define MAX_OPEN_SOCKETS                4               // Concurrent connections (max 4, 3 internal for HTTP server)
#define SERVER_KEEP_ALIVE_TIMEOUT      0               // Disable keep-alive timeout for AP stability
#define SERVER_KEEP_ALIVE_INTERVAL     0               // Disable keep-alive for AP connections

// REST API Endpoints
#define ENDPOINT_STATUS                 "/api/status"
#define ENDPOINT_CONFIG                 "/api/config"
#define ENDPOINT_EMERGENCY              "/api/emergency_stop"
#define ENDPOINT_SETTINGS               "/settings"
#define ENDPOINT_ROOT                   "/"

// ============================================================================
// FreeRTOS Task Configuration
// ============================================================================

// Task Priorities (0-24, higher = more urgent)
#define TASK_PRIO_WATCHDOG              24      // Highest: Watchdog
#define TASK_PRIO_SENSOR                21      // High: Sensor reading
#define TASK_PRIO_VALVE                 22      // Higher: Valve control closes first
#define TASK_PRIO_SERVER                9       // Medium: HTTP server/UI
#define TASK_PRIO_WIFI                  7       // Lower: WiFi monitor + DNS
#define TASK_PRIO_MAIN                  5       // Low: Background work

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
#define TASK_CORE_NETWORK               0       // WiFi, HTTP and DNS stay on network core
#define TASK_CORE_SAFETY                1       // Sensor + valve isolated on control core
#else
#define TASK_CORE_NETWORK               0
#define TASK_CORE_SAFETY                0
#endif

// Task Stack Sizes (bytes)
#define TASK_STACK_SENSOR               4096
#define TASK_STACK_VALVE                2048
#define TASK_STACK_SERVER               8192
#define TASK_STACK_WATCHDOG             2048
#define TASK_STACK_WIFI                 8192    // Increased from 4096 for complex initialization
#define TASK_STACK_TOUCH                3072    // Touch-Key Polling
#define TASK_STACK_STACK_MONITOR        3072    // Stack-Überwachung für Laufzeitwarnungen

// Task Intervals
// (Already defined in Task Configuration section above)

#define TASK_STACK_MONITOR_INTERVAL_MS  2000    // Alle 2s Stack-Reserve prüfen
#define STACK_USAGE_WARNING_PERCENT     96      // UI-Warnung erst bei sehr hoher Stack-Auslastung

// ============================================================================
// Hardware Watchdog Configuration
// ============================================================================

#define WATCHDOG_TIMEOUT_MS             30000   // 30 Sekunden Timeout
#define WATCHDOG_CHECK_INTERVAL_MS      5000    // 5 Sekunden Prüf-Zyklus

// Watchdog-Heartbeat-Keys (für Task-Überwachung)
#define WATCHDOG_KEY_SENSOR             "sensor"
#define WATCHDOG_KEY_VALVE              "valve"
#define WATCHDOG_KEY_SERVER             "server"

// ============================================================================
// Logging & Debug Configuration
// ============================================================================

// Version Configuration (MAJOR.MINOR.PATCH)
// PATCH wird automatisch inkrementiert (BUILD_NUMBER)
// Wenn MAJOR oder MINOR erhöht, wird PATCH auf 0 zurückgesetzt
#define APP_VERSION_MAJOR               0
#define APP_VERSION_MINOR               1
#define APP_VERSION                     "0.1.0"  // MAJOR.MINOR.0 (vor Build-Nummern)
// LOG_LOCAL_LEVEL defined by esp_log.h - do not redefine

// Tag für ESP_LOG_x() Makros
#define TAG_MAIN                        "delongi-main"
#define TAG_SENSOR                      "delongi-sensor"
#define TAG_VALVE                       "delongi-valve"
#define TAG_SERVER                      "delongi-server"
#define TAG_NVS                         "delongi-nvs"
#define TAG_WIFI                        "delongi-wifi"

// ============================================================================
// Safety & Error Handling
// ============================================================================

// Brownout-Level (automatic reset wenn Spannung zu niedrig)
#define BROWNOUT_LEVEL                  BROWNOUT_LEVEL_2

// Fehler-Handling: Was passiert bei kritischen Ausfällen?
#define ERROR_SENSOR_TIMEOUT_MS         5000    // Sensor nicht erreichbar
#define ERROR_WIFI_RETRY_LIMIT          3
#define ERROR_RESTART_ON_WATCHDOG       1       // 1: Auto-restart, 0: Halt

// ============================================================================
// UI/UX Configuration (Web-Interface)
// ============================================================================

#define REFRESH_INTERVAL_MS             1000    // Frontend-Refresh-Rate
#define TANK_GRAPHIC_HEIGHT_PX          200     // CSS: Tankgrafik-Höhe
#define IPHONE15_SCREEN_WIDTH_PX        390     // iPhone15 viewport width

// ============================================================================
// Time & Timing
// ============================================================================

#define TICK_RATE_HZ                    100     // FreeRTOS tick rate

/**
 * === REQUIRED sdkconfig SETTINGS ===
 * 
 * Diese Konstanten benötigen entsprechende Enablements in sdkconfig:
 * 
 * □ mDNS Support:
 *   CONFIG_MDNS_SERVICE_DISCOVERY=y
 *   CONFIG_MDNS_ENABLE_DEBUG=n
 * 
 * □ WiFi AP-Mode mit Static IP:
 *   CONFIG_WIFI_AP_ASSIGN_IP=y
 *   CONFIG_WIFI_SOFTAP_DHCP=n (für statische IP)
 * 
 * □ Network Interface:
 *   CONFIG_ETH_ENABLED=n (nur WiFi nötig)
 *   CONFIG_LWIP_TCPIP_CORE_LOCKING=y
 * 
 * □ HTTP Server:
 *   CONFIG_ESP_HTTP_SERVER=y
 *   CONFIG_HTTPD_MAX_URI_LEN=512
 *   CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
 * 
 * Später auszufüllen: Button im VS Code für menuconfig
 */

#endif // CONFIG_H
