# delonghi-tank: Phase 3 Integration Guide
## WiFi + HTTP REST API Implementation

---

## 📋 Überblick

Phase 3 fügt dem delonghi-tank eine vollständige REST API via WiFi hinzu. Das System läuft jetzt:
- **Phase 1**: Sensoren & Valve Control (lokal)
- **Phase 2**: Automatischer Betrieb (Auto/Manual)
- **Phase 3** ✨ **NEUE**: Web-API + WiFi + Mobiltelefon-Steuerung

---

## 🏗️ Architektur

```
┌──────────────────────────────────────────────────┐
│                 FreeRTOS Kernel                   │
├──────────────────────────────────────────────────┤
│                                                   │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  │
│  │ Sensor Task│  │ Valve Task │  │ WiFi Task  │  │
│  │ (50ms)     │  │ (100ms)    │  │ (HTTP)     │  │
│  └────────────┘  └────────────┘  └────────────┘  │
│        │                │               │         │
│        │                │        ┌──────┴──────┐  │
│        │                │        │              │  │
│        └────────────────┴────────┤ sys_state    │  │
│                                  │ (Shared)     │  │
│        ┌──────────────────────────┤              │  │
│        │        I2C (Sensor)      │              │  │
│        │        GPIO (Valve)      └──────────────┘  │
│        │        SPI (NVS)                           │
│        │                                            │
│        └─ Drivers                                   │
│                                                   │
│        ┌─────────────────────────────┐            │
│        │  HTTP Server               │            │
│        │  - /api/status → JSON      │            │
│        │  - /api/config → JSON      │            │
│        │  - /api/valve/* → Control  │            │
│        │  - /api/emergency_stop     │            │
│        │  - / → HTML Web UI         │            │
│        └─────────────────────────────┘            │
│                    ↓                               │
│        ┌─────────────────────────────┐            │
│        │  mDNS: delonghi-tank.local   │            │
│        │  SNTP: Zeit Sync            │            │
│        └─────────────────────────────┘            │
└──────────────────────────────────────────────────┘
         ↓ WiFi (STA+AP)
    ┌────────────────────────────────────┐
    │     WiFi-Netzwerk                  │
    ├────────────────────────────────────┤
    │ • Mobile App                       │
    │ • Web Browser                      │
    │ • Home Automation (MQTT)           │
    │ • Cloud Integration (REST)         │
    └────────────────────────────────────┘
```

---

## 🔧 Dateien Integration

### 1. **CMakeLists.txt anpassen**

Zur bestehenden `CMakeLists.txt` hinzufügen:

```cmake
idf_component_register(
    SRCS 
        main.c
        wifi_net.c            # ← NEW
        http_handlers.c       # ← NEW
    
    REQUIRES
        # ... existing ...
        esp_wifi              # ← NEW
        esp_netif             # ← NEW
        esp_event             # ← NEW
        esp_http_server       # ← NEW
        mdns                  # ← NEW
        esp_sntp              # ← NEW
)
```

### 2. **Header Deklaration in main.h**

```c
// WiFi functions (from wifi_net.c)
extern esp_err_t init_wifi(void);
extern const char *get_wifi_status_string(void);

// HTTP handlers (from http_handlers.c)
extern httpd_handle_t start_webserver(void);
extern esp_err_t status_handler(httpd_req_t *req);
extern esp_err_t config_get_handler(httpd_req_t *req);
// ... etc
```

### 3. **main.c aktualisieren**

```c
// In app_main():
void app_main(void)
{
    // ... Phase 1 Initialization ...
    
    // Phase 3: WiFi Task
    xTaskCreate(
        wifi_task,
        "wifi_task",
        TASK_STACK_WIFI,
        NULL,
        TASK_PRIO_MAIN,
        &wifi_task_handle
    );
}

// Neue Task hinzufügen:
static void wifi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi task started");
    
    if (init_wifi() == ESP_OK) {
        httpd_handle_t server = start_webserver();
        if (server) {
            ESP_LOGI(TAG, "HTTP Server running");
        }
    }
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

---

## 🚀 Build & Deployment

### Build für Phase 3

```bash
# 1. Build
idf.py build

# 2. Flash mit Bootloader + App
idf.py flash

# 3. Monitor
idf.py monitor

# Erwartete Log-Ausgabe:
# [INFO] WiFi initialization complete
# [INFO] HTTP server started successfully
# [INFO] Access from: http://10.1.1.1 (AP mode)
```

### Schneller Upload (nach Initial Setup)

```bash
# Nach dem ersten Mal - nur App Update (~3 sec)
idf.py app-flash monitor
```

---

## 📱 API Dokumentation

### Allgemein

| Feature | Port | Protokoll |
|---------|------|-----------|
| HTTP API | 80 | REST / JSON |
| mDNS | 5353 | UDP |
| SNTP | 123 | UDP |

### Endpoints

#### 1️⃣ **GET /api/status** - System Status

```bash
curl http://10.1.1.1/api/status
```

**Response:**
```json
{
  "status": "OK",
  "timestamp": 1234567890,
  "device_id": "delonghi-tank-001",
  "sensors": {
    "tank_level_cm": 145,
    "tank_full": 72.5
  },
  "valve": {
    "state": "CLOSED",
    "mode": "AUTO"
  },
  "system": {
    "uptime_seconds": 3600,
    "free_heap_bytes": 98304,
    "temperature_celsius": 45.3
  }
}
```

#### 2️⃣ **GET /api/config** - Konfiguration lesen

```bash
curl http://10.1.1.1/api/config
```

**Response:**
```json
{
  "config": {
    "threshold_top_cm": 200,
    "threshold_bottom_cm": 50,
    "timeout_max_ms": 30000,
    "mode_auto": true,
    "wifi": {
      "ssid": "delonghi-tank",
      "mode": "AP"
    }
  }
}
```

#### 3️⃣ **POST /api/config** - Konfiguration schreiben

```bash
curl -X POST http://10.1.1.1/api/config \
  -H "Content-Type: application/json" \
  -d '{"threshold_top": 250, "threshold_bottom": 40, "timeout_max": 25000}'
```

#### 4️⃣ **POST /api/valve/manual** - Ventil manuell steuern

```bash
# Ventil ÖFFNEN
curl -X POST http://10.1.1.1/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action": "open"}'

# Ventil SCHLIESSEN
curl -X POST http://10.1.1.1/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action": "close"}'
```

#### 5️⃣ **POST /api/emergency_stop** - Notfall-Stopp

```bash
curl -X POST http://10.1.1.1/api/emergency_stop
```

**Effekt:**
- Ventil wird sofort geschlossen
- Alle Operationen gestoppt
- Rotes LED an
- JSON Response mit Status

#### 6️⃣ **GET /** - Web UI

```bash
# Öffnet HTML Interface im Browser
http://10.1.1.1/
```

---

## 🧪 Testszenarien

### Test 1: Basic Connectivity

```bash
# WiFi AP anschauen
# SSID: delonghi-tank-ap
# PW: 12345678
# IP: 10.1.1.1

ping 10.1.1.1

# mDNS Test (nach STA connection)
ping delonghi-tank.local
```

### Test 2: Status API

```javascript
// In Browser Console:
fetch('http://10.1.1.1/api/status')
  .then(r => r.json())
  .then(d => console.log(d))
  .catch(e => console.error('Error:', e))
```

### Test 3: Ventil Kontrolle

```bash
#!/bin/bash

IP="10.1.1.1"

echo "Opening valve..."
curl -X POST http://$IP/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action":"open"}'

sleep 2

echo "Closing valve..."
curl -X POST http://$IP/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action":"close"}'
```

### Test 4: Monitoring Loop

```bash
#!/bin/bash
# Alle 2 Sekunden Status abrufen

while true; do
  curl -s http://10.1.1.1/api/status | jq '.sensors'
  sleep 2
done
```

---

## 🔐 Sicherheit & Optimierungen (Phase 4)

Nach Phase 3 sind folgende Verbesserungen möglich:

- [ ] **HTTPS/TLS** - Verschlüsselte API
- [ ] **API Key Auth** - Simple Token-basierte Auth
- [ ] **MQTT** - Sensor Data Publishing
- [ ] **OTA Updates** - Firmware via WiFi
- [ ] **WebSocket** - Real-time Data Push
- [ ] **Logging Server** - Remote Logging

---

## 🐛 Debugging

### Serial Monitor Output

```
I (456) WIFI_NET: WiFi initialization complete
I (457) WIFI_NET:   AP:   delonghi-tank-ap / 12345678 @ 10.1.1.1
I (458) RADIO: RTOS: Starting RTOS task 0xC
I (465) HTTP_API: Starting HTTP server on port 80
I (501) HTTP_API: HTTP server started successfully
```

### HTTP Server Errors

```c
// Enable debug in main CMakeLists.txt:
target_compile_options(${COMPONENT_LIB} PRIVATE -DENABLE_HTTP_SERVER_DEBUG)
```

### mDNS Test

```bash
# Windows: Install Bonjour (iTunes)
# dann: ping delonghi-tank.local

# Linux: install avahi-utils
avahi-browse -a | grep delonghi
```

---

## 📊 Performance Metrics

| Metrik | Wert | Bemerkung |
|--------|------|----------|
| WiFi Connection | ~2-3s | Nach Startup |
| HTTP Response | <100ms | Typical |
| Memory (WiFi+HTTP) | ~50KB | RAM usage |
| Simultaneous Connections | 7 | Max sockets |
| JSON Parse Time | <5ms | Config endpoint |

---

## 🔗 Nächste Schritte

1. **Testing**: Alle Endpoints via Postman/curl testen
2. **Mobile App**: Flutter/React Native App entwickeln
3. **Cloud Integration**: Firebase oder eigener Server
4. **Security**: TLS und API Key Authentication
5. **Monitoring**: Prometheus/Grafana Integration

---

## 📝 Entwicklungs-Checkliste

- [ ] CMakeLists.txt angepasst
- [ ] wifi_net.c -> main.c integriert
- [ ] http_handlers.c kompiliert
- [ ] Build erfolgreich
- [ ] Flash durchgeführt
- [ ] AP Verbindung getestet
- [ ] /api/status aufgerufen
- [ ] Ventil via API gesteuert
- [ ] WebUI lädt
- [ ] mDNS funktioniert

---

**Version**: Phase 3 (20.1.2025)  
**Status**: ✅ Fertig zur Integration
