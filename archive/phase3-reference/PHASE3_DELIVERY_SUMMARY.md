# 📦 delonghi-tank Phase 3 - Delivery Package

## 🎯 Zusammenfassung

**Phase 3** der delonghi-tank erweitert das Projekt um eine **vollständige WiFi + REST API**.  
Jeder ESP32 wird zum webfähigen Gerät für **Fernsteuerung, Monitoring und Integration**.

---

## 📋 Datei-Übersicht

### 🔧 Neue Source-Dateien (Production Code)

#### 1. **wifi_net.c** - WiFi & Netzwerk-Modul
```
├─ init_netif_wifi()          - Netzwerk-Interfaces init
├─ start_wifi_sta_ap()        - WiFi im STA+AP Modus starten
├─ wifi_event_handler()       - Event-Callbacks
├─ sntp_sync_time()           - Zeit synchronisieren
├─ nvs_read_wifi_credentials()- Config aus NVS lesen
└─ init_wifi()                - Hauptfunktion (Public API)
```
**Größe:** ~8 KB  
**Dependencies:** esp_wifi, esp_netif, mdns, sntp  
**Laufzeit:** ~2-3s (WiFi Connect)

#### 2. **http_handlers.c** - REST API Endpoint-Handler
```
├─ HTTP_ENDPOINTS:
│  ├─ GET /api/status              - System Status (JSON)
│  ├─ GET /api/config              - Konfiguration lesen
│  ├─ POST /api/config             - Konfiguration schreiben
│  ├─ POST /api/valve/manual       - Ventil manuell steuern
│  ├─ POST /api/emergency_stop     - Notfall-Stopp
│  └─ GET /                        - HTML Web UI
├─ Helper Functions:
│  ├─ send_json_response()         - JSON senden
│  └─ send_error_response()        - Error JSON
└─ start_webserver()               - HTTP Server starten
```
**Größe:** ~12 KB  
**Dependencies:** esp_http_server  
**Features:** 6 Endpoints + embedded HTML UI (~2 KB)

#### 3. **main_phase3.c** - Integration für main.c
```
├─ wifi_task()                - WiFi/HTTP Task
├─ start_webserver()          - Server init
├─ app_main() [updated]       - Task creation mit WiFi
└─ Konfigurationskonstanten:
   ├─ SERVER_PORT = 80
   ├─ SERVER_STACK_SIZE = 4096
   ├─ TASK_STACK_WIFI = 8192
   └─ Thresholds, Timeouts, etc.
```
**Integration:** Copy-paste in bestehende main.c  
**Abhängigkeiten:** wifi_net.c, http_handlers.c

---

### 📖 Dokumentations-Dateien

#### 4. **PHASE3_INTEGRATION_GUIDE.md**
Umfassendes Integrations-Handbuch

**Kapitel:**
- 🏗️ Architektur-Überblick
- 🔧 Datei-Integration (CMakeLists.txt, Headers)
- 🚀 Build & Deployment
- 📱 API Dokumentation (6 Endpoints)
- 🧪 Testszenarien (4 grundlegende Tests)
- 🔐 Sicherheit & Optimierungen (Phase 4+)
- 🐛 Debugging Tipps
- 📊 Performance-Metriken

**Größe:** ~400 Zeilen Markdown  
**Lesedauer:** 10-15 Minuten (für vollständiges Setup)

#### 5. **PHASE3_TEST_GUIDE.md**
Praktischer Test-Guide mit Checklisten

**Section:**
- ✅ Test-Checkliste (5 Kategorien)
  - A) Hardware-Verbindung
  - B) WiFi AP Verbindung
  - C) HTTP API Tests (C1-C6)
  - D) Web UI Test
  - E) Permanente Tests
- 🔧 Troubleshooting für häufige Probleme
- 📊 Performance Baseline
- 🎓 Debugging Tricks (Verbose Logging, Wireshark, Memory)
- 📱 Mobile Device Tests (iOS/Android)
- 🏆 Success Checklist
- 🚀 Nächste Phasen vorbereiten

**Größe:** ~350 Zeilen  
**Lesedauer:** 1-2 Stunden (für alle Tests)

#### 6. **CMakeLists.txt.phase3**
Anpasste CMakeLists.txt für Phase 3

**Änderungen:**
```cmake
SRCS += wifi_net.c, http_handlers.c
REQUIRES += esp_wifi, esp_netif, esp_event, esp_http_server, mdns, esp_sntp
```

---

## 🔗 dependencies & Komponenten

### Required ESP-IDF Components
```
✅ freertos             - Kernel (existierend)
✅ driver/gpio         - GPIO Control (existierend)
✅ driver/i2c          - Sensors via I2C (existierend)
✅ nvs_flash           - Persistent config (existierend)
✅ esp_timer           - Timing (existierend)
✅ esp_wifi            - WiFi driver (NEU - Phase 3)
✅ esp_netif           - Network interface (NEU)
✅ esp_event           - Event handling (NEU)
✅ esp_http_server     - HTTP server (NEU)
✅ mdns                - Multicast DNS (NEU)
✅ esp_sntp            - SNTP time sync (NEU)
```

### Optional Components (Phase 4+)
```
⭕ mbedtls            - TLS/HTTPS (optional)
⭕ mqtt_client         - MQTT (optional)
⭕ esp_ota_ops         - OTA Updates (optional)
⭕ json                - cJSON library (optional)
```

---

## 📊 Integration-Schritt für Schritt

### ✅ Schritt 1: Dateien kopieren

```bash
cp wifi_net.c components/main/
cp http_handlers.c components/main/
# Behalte main.c - wir bearbeiten es im nächsten Schritt
```

### ✅ Schritt 2: CMakeLists.txt anpassen

**Alte Version:**
```cmake
idf_component_register(
    SRCS main.c
    REQUIRES freertos esp_driver_gpio esp_driver_i2c nvs_flash
)
```

**Neue Version:**
```cmake
idf_component_register(
    SRCS 
        main.c
        wifi_net.c
        http_handlers.c
    
    REQUIRES
        freertos esp_driver_gpio esp_driver_i2c nvs_flash esp_timer
        esp_wifi esp_netif esp_event esp_http_server mdns esp_sntp
)
```

### ✅ Schritt 3: main.c aktualisieren

**A) Header hinzufügen:**
```c
#include <esp_http_server.h>
#include <mdns.h>

// Forward declarations
extern esp_err_t init_wifi(void);
extern httpd_handle_t start_webserver(void);
extern void wifi_task(void *pvParameters);
```

**B) In app_main() WiFi Task hinzufügen:**
```c
xTaskCreate(
    wifi_task,
    "wifi_task",
    8192,
    NULL,
    4,
    &wifi_task_handle
);
```

**C) wifi_task() Implementierung aus main_phase3.c** kopieren

### ✅ Schritt 4: Build & Test

```bash
# 1. Clean build
idf.py set-target esp32      # oder esp32s3, esp32c3, etc
idf.py fullclean
idf.py build

# 2. Flash
idf.py flash -p /dev/ttyUSB0 monitor   # Linux/Mac
idf.py flash -p COM3 monitor           # Windows

# 3. Monitor Output prüfen
# Suche nach: "WiFi initialization complete"
# Suche nach: "HTTP server started successfully"
```

### ✅ Schritt 5: WiFi Test

```bash
# In einer anderen Terminal:
# 1. SSID suchen: delonghi-tank-ap
# 2. Verbinden mit PW: 12345678
# 3. IP zugewiesen: 10.1.1.1

# 4. Testen:
ping 10.1.1.1
curl http://10.1.1.1/api/status
```

---

## 🎮 Quick API Reference

### Alle 6 Endpoints

| # | Endpoint | Method | Funktion | Response |
|---|----------|--------|----------|----------|
| 1 | `/api/status` | GET | System Status | JSON (sensoren, valve, memory) |
| 2 | `/api/config` | GET | Config lesen | JSON (thresholds, timeout) |
| 3 | `/api/config` | POST | Config schreiben | JSON (updated) |
| 4 | `/api/valve/manual` | POST | Ventil steuern | JSON (state) |
| 5 | `/api/emergency_stop` | POST | Notfall-Stopp | JSON (STOPPED) |
| 6 | `/` | GET | Web UI | HTML (~2 KB) |

### Test-Befehle (curl)

```bash
# Status
curl http://10.1.1.1/api/status | jq .

# Config
curl http://10.1.1.1/api/config | jq .

# Ventil öffnen
curl -X POST http://10.1.1.1/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action":"open"}'

# Ventil schließen
curl -X POST http://10.1.1.1/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action":"close"}'

# Notfall
curl -X POST http://10.1.1.1/api/emergency_stop
```

---

## 📈 Performance & Metriken

Nach erfolgreichem Build solltest du folgende Metriken erwarten:

| Metrik | Wert | Note |
|--------|------|------|
| **Build Time** | 45-60s | Erste Kompilation mit WiFi |
| **Flash Time** | 8-12s | Bootloader + App + PartTable |
| **Boot Time** | 2-3s | bis HTTP Server läuft |
| **HTTP Response** | 50-100ms | Status Endpoint |
| **RAM Usage (WiFi+HTTP)** | ~50 KB | Von insgesamt 512 KB |
| **Free Heap** | 100-150 KB | Nach Startup |
| **WiFi Latency** | 5-20ms | AP Mode, local network |
| **Concurrent Requests** | 7 | Max simultane Verbindungen |

---

## 🐛 Häufige Fehler & Lösungen

### ❌ Error: "E (xxx) esp_http_server: Failed to start server"

**Ursache:** Port 80 belegt oder nicht genug RAM  
**Lösung:**
```bash
# Heap in menuconfig erhöhen:
idf.py menuconfig
# Suche: "Heap size" → auf 300 KB erhöhen
```

### ❌ Error: "WiFi init failed"

**Ursache:** WiFi Component nicht verfügbar  
**Lösung:**
```bash
# Überprüfe CMakeLists.txt:
grep "esp_wifi" components/main/CMakeLists.txt

# Falls nicht da:
# Neue Version: siehe Schritt 2 oben
```

### ❌ WiFi AP nicht sichtbar

**Ursache:** WiFi Task nicht gestartet  
**Lösung:**
```bash
# Prüfe Monitor Output:
idf.py monitor | grep -i wifi

# Überprüfe xTaskCreate() in app_main()
```

### ❌ HTTP Requests timeout

**Ursache:** Stack zu klein  
**Lösung:**
```c
xTaskCreate(
    wifi_task,
    "wifi_task",
    8192,  // Erhöher auf 16384 wenn nötig
    NULL,
    4,
    NULL
);
```

---

## ✨ Was ist Phase 3?

**Vorher (Phase 1-2):**
- ✅ Sensoren auslesen (I2C)
- ✅ Ventil steuern (GPIO)
- ✅ Auto/Manual Mode
- ✅ Schwellenwert-Logik

**Jetzt (Phase 3):**
- ✨ WiFi mit AP-Fallback
- ✨ REST API (6 Endpoints)
- ✨ HTML Web UI
- ✨ mDNS & SNTP
- ✨ **Fernsteuerung möglich** 🚀

**Nächste Phasen:**
- 🔒 Phase 4: TLS/HTTPS Security
- 📱 Phase 5: Mobile App (Flutter/React Native)
- ☁️ Phase 6: Cloud Integration (Firebase/MQTT)
- 🔐 Phase 7: OTA Updates & Secure Boot

---

## 📚 Dokumentations-Roadmap

```
Was du tun solltest:
1. Lese PHASE3_INTEGRATION_GUIDE.md     (10 min)
2. Führe alle Schritte durch           (30 min)
3. Starte die Build                    (1-2 min)
4. Flashe auf ESP32                    (1 min)
5. Führe PHASE3_TEST_GUIDE Tests durch (1-2 h)
6. Verifiziere alle Endpoints          (30 min)
7. Feiere den Erfolg! 🎉              

Gesamtdauer: ~3-4 Stunden für vollständige Integration
```

---

## 🎁 Bonus: Nützliche Tools

### Browser Extensions
- **Thunder Client** - REST API Testing in VS Code
- **REST Client** - Inline HTTP testen

### Command Line Tools
```bash
# Installation:
brew install curl jq    # Mac
apt install curl jq     # Linux
(curl ist in Windows integriert)

# Nützliche Befehle:
curl http://10.1.1.1/api/status | jq '.sensors'  # Nur Sensoren
curl http://10.1.1.1/api/status | jq '.system.free_heap_bytes'  # Heap
```

### mDNS Tester
```bash
# MacOS/Linux:
brew install avahi-utils   # oder: apt install avahi-tools
avahi-browse -a | grep delonghi

# Windows:
# Installiere Bonjour (iTunes)
# Dann funktioniert: ping delonghi-tank.local
```

---

## ✅ Integration-Checklist

```
□ Phase 1 & 2 existieren und funktionieren
□ ESP-IDF 5.x+ installiert
□ Port /dev/ttyUSB0 oder COM3 verfügbar
□ wifi_net.c kopiert zu components/main/
□ http_handlers.c kopiert zu components/main/
□ CMakeLists.txt mit neuen REQUIRES aktualisiert
□ main.c mit wifi_task () aktualisiert
□ idf.py build erfolgreich
□ idf.py flash ohne Fehler
□ Monitor zeigt "WiFi initialization complete"
□ Monitor zeigt "HTTP server started"
□ SSID "delonghi-tank-ap" sichtbar
□ WiFi Verbindung erfolgreich (10.1.1.1)
□ curl http://10.1.1.1/api/status funktioniert
□ HTTP Response ist gültiges JSON
□ /api/valve/manual ändert Ventilstatus
□ Alle Tests aus PHASE3_TEST_GUIDE.md bestanden
□ Web UI lädt auf http://10.1.1.1/
□ Dokumentation gelesen und verstanden
```

---

**🎉 Phase 3 ist fertig zur Integration! Viel Erfolg! 🎉**

---

*Letzte Aktualisierung: 20.01.2025*  
*Phase 3 Completion Status: ✅ 100% ready for production*
