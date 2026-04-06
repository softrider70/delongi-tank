# delonghi-tank Phase 3: Praktischer Test-Guide

## 🎯 Quick Start

Nach dem Flash auf dem ESP32:

```
1. ESP32 startet
2. WiFi AP aktiviert → SSID "delonghi-tank-ap"
3. HTTP Server läuft auf 10.1.1.1
4. Verbinde dich mit dem AP
5. Öffne http://10.1.1.1 im Browser
```

---

## ✅ Test-Checkliste (Schritt-für-Schritt)

### A) Hardware Verbindung prüfen

- [ ] USB-Kabel angeschlossen
- [ ] Serial Port im Terminal sichtbar (z.B. COM3, /dev/ttyUSB0)
- [ ] Build erfolgreich: `idf.py build`
- [ ] Flash erfolgreich: `idf.py flash`
- [ ] Monitor zeigt Boot Log: `idf.py monitor`

**Erwartete Log-Ausgabe:**
```
I (456) WIFI_NET: WiFi initialization complete
I (457) WIFI_NET:   AP:   delonghi-tank-ap / 12345678 @ 10.1.1.1
I (465) HTTP_API: Starting HTTP server on port 80
I (501) HTTP_API: HTTP server started successfully
```

### B) WiFi AP Verbindung testen

| Test | Aktion | Ergebnis |
|------|--------|----------|
| AP visible | In WiFi-Liste suchen | SSID "delonghi-tank-ap" sichtbar |
| AP connect | Mit PW "12345678" verbinden | Verbindung erfolgreich |
| IP erhalten | `ipconfig getifaddr en0` (Mac) | 10.1.1.x erhalten |
| Ping | `ping 10.1.1.1` | ≤ 5ms Latenz |

### C) HTTP API Tests (via Terminal)

#### Test C1: Status Endpoint

```bash
# GET /api/status - System Status auslesen
curl http://10.1.1.1/api/status

# Erwartet: JSON mit sensor/valve/system Info
# Response Time: < 100ms
```

**Erfolgs-Kriterium:**
```json
{
  "status": "OK",
  "sensors": {
    "tank_level_cm": [0-250],
    "tank_full": [0-100]
  },
  "valve": {
    "state": "OPEN" oder "CLOSED"
  }
}
```

#### Test C2: Config Endpoint (GET)

```bash
curl http://10.1.1.1/api/config

# Soll aktuelle Schwellenwerte zeigen
```

#### Test C3: Config Endpoint (POST) - Schwellenwerte ändern

```bash
# Neue Schwellenwerte setzen
curl -X POST http://10.1.1.1/api/config \
  -H "Content-Type: application/json" \
  -d '{"threshold_top": 220, "threshold_bottom": 60, "timeout_max": 28000}'

# Response: {"status": "OK", "message": "Config updated"}
```

**Verifizieren:** GET /api/config aufrufen → neue Werte sollen angezeigt werden

#### Test C4: Ventil Kontrolle - ÖFFNEN

```bash
curl -X POST http://10.1.1.1/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action": "open"}'
```

**Erfolgs-Kriterium:**
- Response: `{"status": "OK", "action": "open"}`
- ESP32 Log: "Valve opened manually"
- GPIO für Ventil sollte HIGH sein (oder LOW, je nach Konfiguration)
- Ventil physisch geöffnet (wenn mit Motor verbunden)

#### Test C5: Ventil Kontrolle - SCHLIEßEN

```bash
curl -X POST http://10.1.1.1/api/valve/manual \
  -H "Content-Type: application/json" \
  -d '{"action": "close"}'
```

**Erfolgs-Kriterium:**
- Response: `{"status": "OK", "action": "close"}`
- Ventil physisch geschlossen

#### Test C6: Notfall-Stopp

```bash
curl -X POST http://10.1.1.1/api/emergency_stop
```

**Verifizieren:**
- Response: `{"status": "EMERGENCY_STOP", "valve": "CLOSED"}`
- Ventil sofort geschlossen
- Rotes LED aktiv (GPIO_LED_ERROR)
- Weiterer Betrieb nur nach Neustart möglich

### D) Web UI Test

- [ ] Browser öffnet `http://10.1.1.1/`
- [ ] HTML lädt (nicht nur CSS)
- [ ] Tank-Status angezeigt
- [ ] Prozentangabe sichtbar
- [ ] "Öffnen" Button klickbar
- [ ] "Schließen" Button klickbar
- [ ] Ventil reagiert auf Button-Clicks

### E) Permanente Monitoring-Tests

#### Test E1: Status Loop (2 Sekunden)

```bash
#!/bin/bash
echo "Monitoring Status (stopppable Ctrl+C)..."
while true; do
  curl -s http://10.1.1.1/api/status | jq '.sensors.tank_level_cm'
  sleep 2
done
```

**Beobachtung:** Wert sollte sich bei Tankfüllung ändern

#### Test E2: Performance unter Last

```bash
# 10 Requests hintereinander
for i in {1..10}; do
  time curl -s http://10.1.1.1/api/status > /dev/null
  sleep 0.5
done
```

**Erwartet:** Alle <100ms

#### Test E3: Netzwerkstabilität

```bash
# Ping kontinuierlich
ping -c 100 10.1.1.1

# Beobachtung:
# - Keine Lost Packets
# - Konsistente RTT
# - Jitter < 10ms
```

---

## 🔧 Troubleshooting

### Problem: WiFi AP nicht sichtbar

**Lösungen:**
1. Monitor Output prüfen - suche nach "WiFi initialization"
2. Check: `menuconfig` → WiFi enabled?
3. Restart: `idf.py erase-flash && idf.py flash`

### Problem: HTTP Server startet nicht

**Lösungen:**
1. Port 80 belegt? → Andere Geräte deaktivieren
2. heap_size zu klein? → `menuconfig` erhöhen
3. Check Log: "Failed to start HTTP server" für Details

### Problem: JSON Response ist leer

**Lösungen:**
1. Check: esp_timer läuft? → Log nach "timeout"
2. Check: sys_state initialisiert? → init_gpio() erfolgreich?
3. RAM voll? → Free Memory in Status prüfen

### Problem: Curl funktioniert nicht in WSL

```bash
# Windows → WSL Netzwerk Bridge nötig
# CLI: Windows CMD verwenden:
curl http://10.1.1.1/api/status

# Oder WSL in Bridge Mode:
wsl --update
# Windows Settings → WSL Integration aktivieren
```

### Problem: mDNS (delonghi-tank.local) funktioniert nicht

```bash
# Das ist OK für Phase 3 - nur STA Mode unterstützt
# Workaround: 10.1.1.1 verwenden bis STA connected

# Nach STA Connection sollte es funktionieren:
ping delonghi-tank.local
```

---

## 📊 Performance Baseline

Nach erfolgreichem Test sollte dein System folgende Metriken erfüllen:

| Metrik | Min | Typ | Max | Status |
|--------|-----|-----|-----|--------|
| WiFi AP launchtime | - | 2-3s | - | ✅ |
| HTTP Response | - | 50ms | 150ms | ✅ |
| JSON Parse | - | <5ms | 10ms | ✅ |
| Memory (free) | 80KB | 100KB | - | ✅ |
| Ping RTT | 1ms | 3ms | 10ms | ✅ |
| Simultaneous Requests | 1 | 7 | 7 | ✅ |

---

## 🎓 Debugging Tricks

### Verbose Logging

In `main.c`:
```c
// Register HTTP Server Debug Handler
esp_http_server_set_debug(server, 7);  // 7 = max debug level
```

### Wireshark Packet Capture

```bash
# WiFi Packets mitschneiden
tcpdump -i en0 -n tcp.port==80 -w delonghi.pcap

# Später analysieren:
wireshark delonghi.pcap
```

### Memory Profiling

```bash
# Im Status JSON:
{
  "system": {
    "free_heap_bytes": 98304  # Sollte > 80KB sein
  }
}
```

### Valve GPIO Debug

```c
// In valve_task():
ESP_LOGI(TAG, "Valve GPIO = %d", gpio_get_level(GPIO_VALVE_CTRL));
```

---

## 📱 Mobile Device Test (Optional)

### iPhone/iPad Test

```
1. "delonghi-tank-ap" in WiFi SSID wählen
2. Password: 12345678
3. Safari öffnen
4. URL: 10.1.1.1 eingeben
   (Oder: Bookmark für schnellen Access)
5. Web UI sollte laden
```

### Android Test

```
1. WiFi-Einstellungen → "delonghi-tank-ap"
2. Password eingeben
3. Chrome Browser
4. Adresse: 10.1.1.1
5. Testen: Buttons klicken
```

---

## 🏆 Success Checklist

Nach allen Tests sollten diese Punkte ✅ sein:

```
Connectivity:
- [x] WiFi AP mit zentralem Name sichtbar
- [x] IP-Adresse 10.1.1.1 zugewiesen
- [x] Ping funktioniert

HTTP API:
- [x] /api/status JSON Response vollständig
- [x] /api/config GET/POST funktionieren
- [x] /api/valve/manual öffnet/schließt
- [x] /api/emergency_stop reagiert

Web UI:
- [x] HTML lädt im Browser
- [x] Status aktualisiert sich
- [x] Buttons funktionieren

Performance:
- [x] Response Zeit < 100ms
- [x] Keine Lost Packets
- [x] Free Memory > 80KB

System:
- [x] Log zeigt keine Fehler
- [x] Valve physisch kontrollierbar
- [x] Sensoren geben Werte zurück
```

---

## 🚀 Nächste Phase vorbereiten

Nach erfolgreicher Phase 3:

1. **Phase 3.5** (Optional): TLS/HTTPS für sichere API
2. **Phase 4**: Mobile App (Flutter oder React Native)
3. **Phase 5**: Cloud Integration (Firebase oder MQTT)
4. **Phase 6**: Advanced Security (Secure Boot, OTA)

---

**Test durchgeführt am:** _______________  
**Von:** _______________________________  
**Alle Tests erfolgreich:** ☐ Ja ☐ Nein  
**Notizen:** ________________________________________________________________

