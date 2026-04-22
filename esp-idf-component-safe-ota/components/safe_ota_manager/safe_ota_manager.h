/**
 * @file safe_ota_manager.h
 * @brief Header für sichere OTA-Verwaltung mit A/B-Partitionierung
 * 
 * Bietet zentrale Schnittstellen für sichere OTA-Updates:
 * - Watchdog-Integration mit automatischem Fallback
 * - Thread-Safe OTA-Status-Management
 * - NVS-Persistenz für Konfiguration und Tracking
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Konstanten und Konfiguration
// ============================================================================

#define SAFE_OTA_NAMESPACE "safe_ota_config"
#define SAFE_OTA_MAX_PARTITIONS 2
#define SAFE_OTA_PARTITION_PREFIX "ota_"

// ============================================================================
// OTA-Status-Enumeration
// ============================================================================

typedef enum {
    SAFE_OTA_STATE_IDLE,
    SAFE_OTA_STATE_DOWNLOADING,
    SAFE_OTA_STATE_VALIDATING,
    SAFE_OTA_STATE_INSTALLING,
    SAFE_OTA_STATE_SUCCESS,
    SAFE_OTA_STATE_FAILED,
    SAFE_OTA_STATE_ROLLBACK
} safe_ota_state_t;

// ============================================================================
// OTA-Konfigurationsstruktur
// ============================================================================

typedef struct {
    char current_version[32];
    char target_version[32];
    safe_ota_state_t state;
    uint32_t start_time_ms;
    uint32_t progress_percent;
    bool rollback_available;
    bool watchdog_enabled;
    uint32_t timeout_ms;
    uint8_t sha256_hash[32];
} safe_ota_config_t;

// ============================================================================
// Public API-Funktionen
// ============================================================================

/**
 * @brief Safe OTA Manager initialisieren
 */
esp_err_t safe_ota_manager_init(const safe_ota_config_t *config);

/**
 * @brief OTA-Update starten mit erweiterter Sicherheit
 */
esp_err_t safe_ota_start_update(const char *url, const char *version);

/**
 * @brief OTA-Status abrufen
 */
esp_err_t safe_ota_get_status(safe_ota_config_t *status);

/**
 * @brief Manuellen Rollback durchführen
 */
esp_err_t safe_ota_force_rollback(void);

/**
 * @brief Watchdog für OTA-Operationen konfigurieren
 */
esp_err_t safe_ota_configure_watchdog(uint32_t timeout_ms, bool enable_panic_handler);

/**
 * @brief OTA-Validierung konfigurieren
 */
esp_err_t safe_ota_configure_validation(bool enable_sha_check, bool enable_version_check, bool enable_size_check);

/**
 * @brief Partition-Informationen abrufen
 */
esp_err_t safe_ota_get_partition_info(const esp_partition_t **running, const esp_partition_t **ota_0, const esp_partition_t **ota_1);

/**
 * @brief Cleanup und Ressourcen freigeben
 */
void safe_ota_manager_deinit(void);

#ifdef __cplusplus
}
#endif
