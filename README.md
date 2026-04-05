# delongi-tank

ESP32-Firmware fuer die Ueberwachung und Steuerung eines Kaffeemaschinen-Wassertanks mit VL53L0X, Ventilsteuerung, WiFi-Weboberflaeche und persistenter Konfiguration.

## Aktueller Stand

Der aktive Lauf- und Buildpfad liegt in `components/main/main.c`.

Die vorherige Phase-3-Aufteilung auf mehrere Referenzdateien wurde archiviert und ist nicht mehr der aktuelle Buildpfad:

- `archive/phase3-reference/`

## Implementierter Funktionsumfang

### Tank- und Ventillogik

- VL53L0X-Abstandsmessung ueber I2C
- Automatische Befuellung zwischen OBEN- und UNTEN-Schwelle
- Manuelles Befuellen ueber Web-UI und API
- Sicherheitsabschaltung ueber maximale Ventil-Offenzeit
- Zusaetzlicher Fortschritts-Timeout fuer automatische Befuellung
- Keine falsche Notaus-Ausloesung mehr bei manuellem Befuellen ohne Fortschrittsdelta

### Notaus und Sicherheit

- Persistenter Notaus-Status in NVS
- Notaus kann per API und UI explizit ausgeloest und explizit zurueckgesetzt werden
- Notaus schliesst das Ventil sofort und beendet manuelle Befuellung
- Fehlergrund fuer den Notaus wird im Status ausgeliefert

### Konfiguration und Persistenz

- Persistente Speicherung von:
    - `threshold_top_cm`
    - `threshold_bottom_cm`
    - `timeout_max_ms`
    - `fill_progress_timeout_ms`
    - `flow_rate_l_per_min`
    - WiFi-Credentials
    - Notaus-Status
- Durchflusswert ist als konfigurierbarer Parameter vorhanden
- Standardwert fuer Durchfluss: 10.0 L/min

### Web-UI

- Eingebettete HTML/CSS/JS-Oberflaeche direkt in der Firmware
- Dashboard, Settings und WiFi-Bereich
- Anzeige von:
    - App-Version
    - Fuellstand in cm und Prozent
    - Ventilstatus
    - manueller Befuellung
    - WiFi-Status
    - Gesamt-Literzaehler
    - Notaus-Zustand und Fehlergrund
- Eingabe und Validierung fuer:
    - OBEN
    - UNTEN
    - Timeout
    - Fill-Progress-Timeout
    - Durchfluss in L/min

## Konkrete Chat-Anforderungen, die in die Doku uebernommen wurden

Diese Punkte kamen explizit aus dem Arbeitschat und werden jetzt als Projektanforderungen gefuehrt:

- App-/Build-Version muss in der UI sichtbar und aus dem Build ableitbar sein
- Notaus darf sich nicht implizit selbst zuruecksetzen
- Durchflusswert 10 L/min muss in der Konfiguration vorhanden sein
- Ventil- bzw. Mengenzaehler muessen sichtbar sein
- Eingaben in der Konfigurationsseite muessen validiert werden
- Alte, irrefuehrende Parallelimplementierungen duerfen nicht den aktiven Buildpfad verdecken

## API-Uebersicht

Der aktuelle Firmwarepfad registriert folgende Endpunkte:

- `GET /`
- `GET /api/status`
- `GET /api/config`
- `POST /api/config`
- `POST /api/valve/manual`
- `POST /api/emergency_stop`
- `GET /api/wifi/status`
- `POST /api/wifi/config`
- `POST /api/system/reset`

## Statusmodell

`GET /api/status` liefert unter anderem:

- Sensorstatus inklusive Fuellstand in Prozent
- Ventilstatus
- manueller Befuellungsstatus
- Gesamt-Literzaehler
- Notaus-Flag und Notaus-Grund
- App-Version
- WiFi-Verbindungsstatus
- konfigurierte Timeouts und Durchflussparameter

## Validierungsregeln

Die aktuelle Firmware validiert Konfiguration wie folgt:

- OBEN und UNTEN muessen gueltige Zahlen sein
- OBEN muss kleiner als UNTEN sein
- OBEN und UNTEN muessen im erlaubten Zentimeterbereich liegen
- Timeout-Werte muessen mindestens 1000 ms betragen
- Durchfluss muss groesser als 0 und kleiner oder gleich 50 L/min sein

## Netzwerk

- AP-Fallback-IP: `10.1.1.1`
- WiFi-Status und Credential-Update sind ueber dedizierte Endpunkte verfuegbar
- HTTP-Oberflaeche ist sowohl fuer AP- als auch fuer STA-Betrieb vorgesehen

## Build und Flash

### Umgebung aktivieren

```powershell
.\activate-esp-idf.ps1
```

### Build

```powershell
idf.py build
```

### Flash

```powershell
idf.py -p COM3 flash
```

### Monitor

```powershell
idf.py -p COM3 monitor
```

## Relevante Dateien

- `components/main/main.c`: aktive Firmware inkl. API und UI
- `include/config.h`: Konfigurationskonstanten und Endpunkte
- `include/version.h`: generierte Versionsinformation
- `tools/increment_build.py`: Buildnummer-Generierung
- `PROJECT.md`: kompakte Projektspezifikation auf Basis des aktuellen Stands

## Hinweise

- `sdkconfig` enthaelt die aktuell genutzte lokale Build-Konfiguration und kann sich stark aendern.
- Historische Phase-3-Referenzen liegen absichtlich im Archiv, damit die aktive Implementierung klar erkennbar bleibt.
- GPIO pin assignments
- UART baudrates
- WiFi/BLE settings
- Display configurations
- Sensor parameters

### Board Selection

The target board is configured in `sdkconfig` and `sdkconfig.defaults.*`

To change boards:
```bash
idf.py set-target esp32s3
idf.py menuconfig
```

## Development Workflow

1. **Edit code** in `src/main.c` or `include/config.h`
2. **Build:** `/build-project`
3. **Flash:** `/upload-firmware` (fast iteration)
4. **Test & Debug**
5. **Commit:** `/commit` (auto-generates commit message)

## Useful Skills

- **`/build-project`** — Compile firmware
- **`/upload-firmware`** — Fast app update (~3 seconds)
- **`/upload`** — Smart session router
- **`/commit`** — Git commit with auto-message

## Adding Features

Use Copilot skills to extend functionality:

```
/add-ota          Enable OTA firmware updates
/add-webui        Add responsive web dashboard
/add-library      Manage external components
/add-security     Enable Secure Boot, encryption
/setup-ci         GitHub Actions CI/CD
/add-profiling    Performance monitoring
```

## Documentation

- **`PROJECT.md`** — Detailed project specifications
- **`sdkconfig`** — Build configuration (auto-generated)
- **`include/config.h`** — Hardware pin mappings

## Troubleshooting

**Build fails:**
```bash
idf.py fullclean
idf.py build
```

**Flash doesn't work:**
- Check USB connection: `idf.py monitor --no-reset`
- Select port manually: `idf.py -p /dev/ttyUSB0 flash`

**Memory issues:**
- Check heap with `/add-profiling`
- Review `sdkconfig` memory settings
- Use PSRAM if available

## Next Steps

1. Update `PROJECT.md` with hardware details
2. Configure `include/config.h` for your board setup
3. Implement application in `src/main.c`
4. Test with `/upload-firmware`
5. When production-ready, use `/add-security`

---

Generated from ESP32 Template  
For template docs: https://github.com/softrider70/esp32-template
