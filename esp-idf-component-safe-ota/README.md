# Safe OTA Component für ESP-IDF

## 🛡️ ESP-IDF Komponente für sichere OTA-Updates

Dieses Paket enthält wiederverwendbare Komponenten für sichere Over-The-Air Updates auf ESP32.

## 📦 Komponenten

### 1. **safe_ota_manager**
- **Zweck**: Zentralisierte OTA-Verwaltung mit A/B-Partitionierung
- **Features**: 
  - Automatischer Boot-Fallback bei OTA-Fehlern
  - Watchdog-Integration mit Panic-Handler
  - Thread-Safe OTA-Status-Management
  - NVS-Persistenz für Konfiguration

### 2. **safe_gpio_controller**
- **Zweck**: Thread-Safe GPIO-Steuerung ohne Race Conditions
- **Features**:
  - Mutex-geschützte GPIO-Operationen
  - Atomare Zustandsänderungen
  - State-Management mit Persistenz
  - Deadlock-Erkennung und Prävention

### 3. **safe_ota_validator**
- **Zweck**: Erweiterte OTA-Validierung mit Security-Checks
- **Funktionen**:
  - SHA-256 Hash-Validierung
  - Version-Kompatibilitäts-Prüfung
  - Firmware-Size-Validierung
  - Rollback-Logik bei ungültigen Images

### 4. **safe_watchdog**
- **Zweck**: Fortschrittlicher Watchdog mit Monitoring und Recovery
- **Eigenschaften**:
  - Panic-Handler für kontrollierte Neustarts
  - Task-Prioritäten-basierte Überwachung
  - Deadlock-Erkennung mit Zeitstempeln
  - Recovery-Mode für Notfallsituationen

## 🚀 Verwendung in Projekten

### **Als Komponente hinzufügen:**
```cmake
# In main CMakeLists.txt
idf_component_register(safe_ota_manager)
idf_component_register(safe_gpio_controller)
```

### **In bestehendes Projekt integrieren:**
```bash
# Komponenten ins Projekt kopieren
cp -r esp-idf-component-safe-ota/components/* components/

# CMakeLists.txt anpassen
# Die Komponenten werden automatisch gefunden und gelinkt
```

## 🔧 Konfiguration

### **Kconfig-Optionen:**
```kconfig
config SAFE_OTA_ENABLE
    bool "Enable Safe OTA Updates"
    default y
    help "Enable secure OTA with A/B partitioning and watchdog protection"

config SAFE_OTA_TIMEOUT_MS
    int "Watchdog timeout in milliseconds"
    default 10000
    range 5000 30000
    help "Watchdog timeout for OTA operations"

config SAFE_OTA_PANIC_HANDLER
    bool "Enable watchdog panic handler"
    default y
    help "Enable panic handler for controlled restarts"
```

### **Menuconfig-Einstellungen:**
```bash
idf.py menuconfig
# → Component configuration → Safe OTA
```

## 📋 Abhängigkeiten

### **Erforderliche ESP-IDF Komponenten:**
- `nvs_flash` für Konfigurationsspeicher
- `esp_partition` für A/B-Partitionierung
- `esp_ota` für OTA-Operationen
- `esp_task_wdt` für Watchdog-Schutz
- `driver` für GPIO-Operationen
- `freertos` für Task-Management

### **Optionale Abhängigkeiten:**
- `mbedtls` für SHA-256 Hash-Berechnung
- `esp_http_client` für OTA-Downloads

## 🎯 Sicherheitseinstellungen

### **Production-Ready Features:**
- ✅ **A/B-OTA** mit automatischem Fallback
- ✅ **Watchdog-Schutz** mit Panic-Handler
- ✅ **Thread-Safe GPIO** ohne Race Conditions
- ✅ **OTA-Validierung** mit SHA-256 Hash
- ✅ **NVS-Persistenz** für Konfiguration
- ✅ **Factory-Recovery** als letzte Rettung

### **Security-Level: HIGH**
- **Brick-Schutz**: Durch A/B-Partitionierung
- **Boot-Loop-Schutz**: Durch Watchdog-Timeout
- **Data-Integrity**: Durch SHA-256 Validierung
- **Access-Control**: Durch Version-Whitelist
- **Recovery**: Automatisch und manuell

---

**Diese Komponenten bieten Enterprise-Level OTA-Sicherheit für ESP32-Projekte!** 🛡️
