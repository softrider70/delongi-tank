---
project_name: delonghi-tank
author: user
version: 0.1.0
description: |
  Automatisiertes Wassertank-Managementsystem fuer Kaffeemaschinen.
  ESP32-basierte Fuellstandsueberwachung mit ToF-Sensor, automatischer Ventilsteuerung,
  WiFi-Webserver-UI, NVS-basierter Konfigurationsspeicherung und integrierter Notaus-Logik.
  Die aktive Implementierung liegt in components/main/main.c.

target_board: esp32  # ESP32-DEVKITC-V4 (38-pin)
target_version: ESP-IDF 6.0.0

active_runtime:
  entrypoint: components/main/main.c
  archived_reference_path: archive/phase3-reference
  versioning:
    generated_header: include/version.h
    generator: tools/increment_build.py

components:
  # ESP-IDF built-in components
  - name: freertos
  - name: esp_psram
  - name: nvs_flash
  - name: esp_wifi
  - name: esp_http_server
  - name: esp_tls
  - name: driver (uart, i2c, gpio)
  
external_dependencies:
  - VL53L0X (ToF sensor driver - IIC)
  - MOSFET control circuit (GPIO-based PWM/digital)

security:
  nvs_encryption: true       # WiFi credentials persistent speichern
  secure_boot: false         # Spaeter optional
  flash_encryption: false    # Spaeter optional
  watchdog: true             # Framework-/Task-Schutz aktiv
  emergency_stop_persistent: true

hardware:
  board: ESP32-DEVKITC-V4 (38-pin, dual-core, 520KB SRAM)
  power: 5V USB + 12V externe Versorgung
  
  pin_mapping:
    i2c_sda: 21              # VL53L0X Sensor
    i2c_scl: 22              # VL53L0X Sensor
    valve_control: 16        # MOSFET-Modul (12V Magnetventil)
    status_led: 2            # Onboard LED
  
  external_peripherals:
    - VL53L0X Time-of-Flight Sensor (I2C address 0x29)
    - 12V Solenoid Valve (via MOSFET module)
    - 5V Power Supply Module

features:
  - Fuellstandsueberwachung via ToF-Sensor (Abstandsmessung von oben)
  - Automatische Ventilsteuerung (Befuellung/Entleerung)
  - Manuelle Befuellung ueber Web-UI und API
  - WiFi-Konnektivität mit Fallback AP-Modus
  - Web-UI (iPhone15-optimiert, pure HTML/CSS/JS)
  - NVS-Speicherung (Credentials, Grenzwerte, Konfiguration, Notaus-Status)
  - Hardware-Watchdog für Task-Sicherheit
  - Timeout-Schutz beim Befuellen
  - Fill-Progress-Timeout fuer automatische Befuellung
  - Persistente Notaus-Logik mit Fehlergrund
  - Build-/App-Version in Status und UI
  - Gesamt-Literzaehler aus Ventil-Offenzeit und Durchflusswert
  - Brownout-Schutz (sofort Ventil deaktivieren)
  - Stack-Monitoring mit persistenter Warnmeldung
  - Sensor-Recovery (manueller Reinit + Auto-Reinit bei invalid Reads)
  - Diagnosetab mit Sensor- und CPU-Livewerten
  - OTA-Update per REST-Endpoint mit Statusabfrage
  - Umschaltbarer USB/OTA-Flash-Workflow via `tools/flash-mode.ps1`

chat_requirements:
  - App-/Build-Version muss sichtbar und nachvollziehbar sein
  - Notaus darf sich nicht automatisch zuruecksetzen
  - 10 L/min muss als konfigurierbarer Durchflusswert vorhanden sein
  - Zaehler muessen in der UI sichtbar sein
  - Validierung der Konfigurationsseite muss funktionieren
  - Historische Parallelimplementierungen duerfen den aktiven Buildpfad nicht verdecken

api:
  endpoints:
    - GET /
    - GET /api/status
    - GET /api/config
    - POST /api/config
    - POST /api/valve/manual
    - POST /api/valve/stop
    - POST /api/emergency_stop
    - GET /api/wifi/status
    - POST /api/wifi/config
    - POST /api/counters/reset
    - POST /api/warnings/reset
    - POST /api/sensor/reset
    - POST /api/ota/start
    - GET /api/ota/status
    - POST /api/system/reset

config_runtime:
  threshold_top_cm: configurable
  threshold_bottom_cm: configurable
  timeout_max_ms: configurable
  fill_progress_timeout_ms: configurable
  flow_rate_l_per_min:
    configurable: true
    default: 10.0

ui_runtime:
  sections:
    - dashboard
    - settings
    - wifi
    - diagnostics
  displays:
    - tank_level_cm
    - fill_percent
    - app_version
    - emergency_state
    - emergency_reason
    - manual_fill_state
    - total_liters
    - wifi_connected
    - sensor_last_raw_mm
    - sensor_invalid_read_count
    - sensor_fallback_reuse_count
    - sensor_stale
    - cpu_core0_percent
    - cpu_core1_percent
    - cpu_top_task
    - cpu_top_task_percent
    - cpu_task_count
  validation:
    - top_and_bottom_numeric
    - top_smaller_than_bottom
    - timeout_min_1000_ms
    - fill_progress_timeout_min_1000_ms
    - flow_rate_between_0_1_and_50

notes: |
  - Alle Grenzwerte sind zur Laufzeit in der UI einstellbar und persistent.
  - Keep it simple: keine externe Frontend-Toolchain, mobile-first, eingebettete UI.
  - Sensor-Messwertlogik: je kleiner Abstand = Tank voller.
  - OBEN-Grenzwert: Tank VOLL (kleiner Abstand).
  - UNTEN-Grenzwert: Tank LEER (grosser Abstand).
  - Historische Phase-3-Dateien wurden in archive/phase3-reference verschoben.
  - OTA-Partitionierung ist auf `partitions_ota_custom.csv` umgestellt (groessere A/B-App-Slots).
  - OTA kann fuer lokalen VS-Code-Workflow via HTTP oder HTTPS gestartet werden.
  - Flash-Modus ist zur Laufzeit per Script schaltbar: `tools/flash-mode.ps1 -Mode usb|ota`.
