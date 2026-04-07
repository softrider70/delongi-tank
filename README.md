# bosch-tank

ESP32-Firmware fuer die Ueberwachung und Steuerung eines Kaffeemaschinen-Wassertanks mit VL53L0X, Ventilsteuerung, WiFi-Weboberflaeche und persistenter Konfiguration.

## Aktueller Stand

Der aktive Lauf- und Buildpfad liegt in `components/main/main.c`.

Die vorherige Phase-3-Aufteilung auf mehrere Referenzdateien wurde archiviert und ist nicht mehr der aktuelle Buildpfad:

- `archive/phase3-reference/`

## Aktuelle Repository-Änderungen

- Branch `master` wurde in `main` umbenannt.
- Remote `origin/master` wurde entfernt; `origin/main` ist der primäre Branch.
- Das Projekt wurde mit `upstream/main` von `softrider70/delongi-tank` synchronisiert.
- Funktionale Ergänzung: Befüllen stoppt beim Erreichen des OBEN-Grenzwerts.
- Der `BEFUELLEN`-Button ist deaktiviert, wenn der Tank bereits voll ist.
- OTA-Diagnosewerte wurden in den Diagnostics-Tab aufgenommen.
- Lokale OTA-Quellen über HTTP/HTTPS (`http://<host-ip>/bosch-tank.bin`) werden jetzt unterstützt.
- Laufzeitdaten wie Gesamt-Oeffnungszeit, Gesamt-Liter und OTA-Status werden persistent in NVS gespeichert.
- Build-/App-Versionen werden automatisch via `tools/increment_build.py` aus `include/config.h` generiert.
- `backup-before-upstream-merge` bleibt als Sicherungsbranch erhalten.

## Implementierter Funktionsumfang

### Tank- und Ventillogik

- VL53L0X-Abstandsmessung ueber I2C
- Automatische Befuellung zwischen OBEN- und UNTEN-Schwelle
- Beim Erreichen des OBEN-Wertes wird das Befuellen gestoppt; das System muss nicht exakt auf dem Wert halten
- Der BEFUELLEN-Button kann nur gestartet werden, solange der aktuelle Messwert groesser als der OBEN-Schwellenwert ist
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
- `POST /api/valve/stop`
- `POST /api/emergency_stop`
- `GET /api/wifi/status`
- `POST /api/wifi/config`
- `POST /api/counters/reset`
- `POST /api/warnings/reset`
- `POST /api/sensor/reset`
- `POST /api/ota/start`
- `GET /api/ota/status`
- `POST /api/system/reset`


## Statusmodell

`GET /api/status` liefert unter anderem:

- Sensorstatus inklusive Fuellstand in Prozent
- Sensor-Diagnose (`last_raw_mm`, `invalid_read_count`, `fallback_reuse_count`, `stale`)
- Ventilstatus
- manueller Befuellungsstatus
- Gesamt-Literzaehler
- Notaus-Flag und Notaus-Grund
- Stack-Warnstatus und Warnmeldung
- CPU-Laufzeitdaten (`cpu_core0_percent`, `cpu_core1_percent`, `cpu_top_task`, `cpu_top_task_percent`, `cpu_task_count`)
- OTA-Status (`phase`, `message`, `last_error`, `in_progress`, `target_version`)
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

## OTA-Update

- Partitionierung ist OTA-optimiert ueber `partitions_ota_custom.csv`
- `POST /api/ota/start` erwartet `{ "url": "http://.../bosch-tank.bin" }` oder `https://...`
- `GET /api/ota/status` liefert den Laufstatus und Fehlerdetails
- Diagnose-Tab beinhaltet jetzt ein OTA-Update-Feld mit lokalem Binärpfad-Vorschlag
- Erfolgreiches OTA fuehrt automatisch einen Neustart aus
- Fuer lokalen VS-Code-Workflow ist HTTP explizit erlaubt (LAN)

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

### USB/OTA Flash-Umschaltung (empfohlen)

Das Projekt enthaelt `tools/flash-mode.ps1` fuer einen einheitlichen Workflow.

USB:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode usb -UsbPort COM3
```

OTA:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode ota -DeviceIp 192.168.1.50
```

Optional mit expliziter Host-IP:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\flash-mode.ps1 -Mode ota -DeviceIp 192.168.1.50 -HostIp 192.168.1.100 -HttpPort 8070
```

Was der OTA-Modus automatisch macht:

- Build ausfuehren
- lokalen HTTP-Bin-Server starten
- OTA per API triggern
- OTA-Status bis Erfolg/Fehler pollen

## Entwickler-Tools & Wartung

- `tools/flash-mode.ps1`: Vereinheitlichter USB/OTA Flash-Workflow aus dem VS Code Terminal.
- `tools/increment_build.py`: Generiert `include/version.h` und inkrementiert die Buildnummer automatisch.
- `tools/update_hardware_inventory.py`: Ordnet ein ESP32-Gerät im Inventar einem Projekt zu, identifiziert über MAC, Port, Chip und Revision.
- Inventardatei: `c:\Users\win4g\Desktop\esp32-hardware-overview.md`

Beispiel:

```powershell
python tools/update_hardware_inventory.py --mac c8:2e:18:f0:36:50 --project CYD --port COM11 --chip ESP32-D0WD-V3 --revision v3.1
```

### Monitor

```powershell
idf.py -p COM3 monitor
```

## Relevante Dateien

- `components/main/main.c`: aktive Firmware inkl. API und UI
- `include/config.h`: Konfigurationskonstanten und Endpunkte
- `include/version.h`: generierte Versionsinformation
- `partitions_ota_custom.csv`: OTA-optimierte 4MB-Partitionierung (A/B-Slots)
- `tools/flash-mode.ps1`: USB/OTA Flash-Umschalter fuer VS Code Terminal
- `tools/increment_build.py`: Buildnummer-Generierung und `version.h`-Erzeugung
- `tools/update_hardware_inventory.py`: Hardware-Inventarpflege fuer ESP32-Projekte
- `PROJECT.md`: kompakte Projektspezifikation auf Basis des aktuellen Stands

## Hinweise

- `sdkconfig` enthaelt die aktuell genutzte lokale Build-Konfiguration und kann sich stark aendern.
- Historische Phase-3-Referenzen liegen absichtlich im Archiv, damit die aktive Implementierung klar erkennbar bleibt.
- Der Diagnose-Reiter in der UI zeigt Sensor- und CPU-Livewerte.
- Gruene Reset-Taste unterstuetzt: kurzer Druck (Warnung reset), 3s halten (Zaehler reset), Doppeltipp (Sensor-Reinit).
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

1. Edit code in `components/main/main.c` or `include/config.h`
2. Build: `idf.py build`
3. Flash:
    - USB: `tools/flash-mode.ps1 -Mode usb`
    - OTA: `tools/flash-mode.ps1 -Mode ota -DeviceIp <ip>`
4. Test & Debug (`idf.py -p COM3 monitor`)
5. Commit changes

## Runtime-Highlights

- Fallback-AP wird nur bei fehlgeschlagenen STA-Reconnects aktiviert
- DNS-Captive-Portal startet/stopt passend zum AP-Modus
- Stack-Monitoring mit persistenter UI-Warnmeldung
- Sensor-Auto-Recovery (Auto-Reinit bei wiederholten invalid Reads)
- iOS-optimierte 4-Tab-Oberflaeche (Dashboard, Settings, WiFi, Diagnose)

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

1. OTA-Bedienung direkt in Diagnose-UI ergaenzen (URL + Start + Live-Status)
2. Optional HTTPS-Zertifikatspruefung fuer OTA-Hardening aktivieren
3. Optional VS Code `tasks.json` fuer 1-Klick USB/OTA-Task anlegen

---

Generated from ESP32 Template  
For template docs: https://github.com/softrider70/esp32-template

