# ESP32 Flash Anleitung (USB) - bosch-tank

Exakte Schritt-für-Schritt Anleitung zum Flashen der ESP32-Firmware über USB unter Windows.

## Voraussetzungen

1. **ESP32-D Board** mit USB-Kabel verbunden
2. **ESP-IDF 6.1** installiert
3. **COM-Port** bekannt (z.B. COM3)
4. **Projekt-Verzeichnis** (bosch-tank)

## Schritt-für-Schritt Anleitung

### 1. ESP-IDF Umgebung aktivieren

```bash
# In neuem Terminal/CMD
cmd /c "C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf\export.bat"
```

**Erwartete Ausgabe:**
```
Activating ESP-IDF 6.1
Setting IDF_PATH to 'C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf'.
* Checking python version ... 3.14.3
* Checking python dependencies ... OK
* Deactivating the current ESP-IDF environment (if any) ... OK
* Establishing a new ESP-IDF environment ... OK
Done! You can now compile ESP-IDF projects.
```

### 2. Projekt-Verzeichnis wechseln

```bash
cd C:\Users\win4g\Downloads\GitHub\VS-Projekte\bosch-tank
```

### 3. Build durchführen

```bash
idf.py build
```

**Erwartete Ausgabe:**
```
Executing action: all (aliases: build)
Running ninja in directory C:\Users\win4g\Downloads\GitHub\VS-Projekte\bosch-tank\build
...
Project build complete. To flash, run:
 idf.py flash
```

### 4. ESP32 in Flash-Modus setzen

**WICHTIG:** Der ESP32 muss im Download-Modus sein!

#### Methode A - BOOT-Button (empfohlen):
1. **BOOT-Button gedrückt halten**
2. **ESP32 einstecken** (oder RESET-Button drücken)
3. **BOOT-Button loslassen** nach 1-2 Sekunden

#### Methode B - RESET-Button:
1. **BOOT-Button gedrückt halten**
2. **RESET-Button kurz drücken**
3. **BOOT-Button loslassen**

**Hinweis:** Der BOOT-Button ist der kleine Button neben dem USB-Port auf dem ESP32-Board.

### 5. Flash durchführen

#### Mit explizitem COM-Port:
```bash
idf.py -p COM3 flash
```

#### Oder COM-Port automatisch finden:
```bash
idf.py flash
```

### 6. Flash-Überwachung (optional)

#### Flash + Serial-Monitor:
```bash
idf.py -p COM3 flash monitor
```

#### Nur Monitor nach Flash:
```bash
idf.py -p COM3 monitor
```

**Monitor beenden:** `Strg + ]`

## Erwartete Ausgabe beim Flash

```
Connecting....
Connected to ESP32 on COM3:
Chip type:          ESP32-D0WD-V3 (revision v3.1)
Features:           Wi-Fi, BT, Dual Core + LP Core, 240MHz
Crystal frequency:  40MHz
MAC:                6c:c8:40:5b:f6:ac

Uploading stub flasher...
Running stub flasher...
Stub flasher running.
Changing baud rate to 460800...
Changed.

Configuring flash size...

Writing 'bootloader/bootloader.bin' at 0x00001000...
SHA digest in image updated.
No changed sectors found, verifying if data is in flash...
'bootloader/bootloader.bin' at 0x00001000 verified.

Writing 'partition_table/partition-table.bin' at 0x00008000...
No changed sectors found, verifying if data is in flash...
'partition_table/partition-table.bin' at 0x00008000 verified.

Writing 'ota_data_initial.bin' at 0x0000f000...
Writing at 0x0000f000 [==============================] 100.0% 31/31 bytes...
Wrote 8192 bytes (31 compressed) at 0x0000f000 in 0.1 seconds (781.4 kbit/s).

Writing 'bosch-tank.bin' at 0x00020000...
Changed data sectors found, fast reflashing...
Writing at 0x00020000 [==============================] 100.0% 1927/1927 bytes...

Hard resetting via RTS pin...
Done
```

## Troubleshooting

### Problem: `Could not open COM3, the port is busy`

**Lösung 1 - COM-Port finden:**
```bash
python -m esptool --chip esp32 flash-id
```

**Lösung 2 - Geräte-Manager:**
1. Windows + R → `devmgmt.msc`
2. "Anschlüsse (COM und LPT)" erweitern
3. COM-Port des ESP32 suchen

### Problem: `Failed to connect to ESP32`

**Lösungen:**
1. **BOOT-Button korrekt halten** (während des Verbindens)
2. **USB-Kabel prüfen** (data-kabel, nicht nur charging)
3. **Anderen USB-Port versuchen**
4. **Treiber neu installieren** (CP210x oder CH340)

### Problem: `idf.py: command not found`

**Lösung:**
```bash
# ESP-IDF Umgebung nicht aktiviert
cmd /c "C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf\export.bat"
```

### Problem: Build nach ESP-IDF Update fehlerhaft

**Lösung:**
```bash
idf.py fullclean
idf.py build
```

## Schnell-Workflow (für erfahrene Nutzer)

```bash
# 1. Umgebung aktivieren
cmd /c "C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf\export.bat"

# 2. In Projekt-Verzeichnis wechseln
cd bosch-tank

# 3. Build + Flash + Monitor
idf.py -p COM3 flash monitor
```

## Erfolgs-Indikatoren

- ✅ **Connecting....** → ESP32 gefunden
- ✅ **Hard resetting via RTS pin...** → Flash abgeschlossen
- ✅ **Done** → Vorgang erfolgreich
- ✅ **ESP32 startet neu** → Firmware läuft

## Wichtige Hinweise

1. **BOOT-Button:** Muss beim Flash-Versuch gedrückt sein
2. **USB-Kabel:** Muss ein Datenkabel sein (kein reines Ladekabel)
3. **COM-Port:** Kann sich ändern, bei Problemen neu prüfen
4. **Zeit:** Der gesamte Prozess dauert ca. 30-60 Sekunden
5. **Build-Nummer:** Wird automatisch von build-and-commit.ps1 erhöht

## Automatisierung (Optional)

Für häufige Updates kann ein Batch-File erstellt werden:

**flash.bat:**
```batch
@echo off
echo ESP32 Flash - bosch-tank
echo.

echo 1. ESP-IDF Umgebung aktivieren...
cmd /c "C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf\export.bat"

echo 2. In Projekt-Verzeichnis wechseln...
cd /d "C:\Users\win4g\Downloads\GitHub\VS-Projekte\bosch-tank"

echo 3. Build und Flash...
idf.py -p COM3 flash monitor

pause
```

**Ausführung:** `flash.bat`

---

**Letzte Aktualisierung:** BUILD 12  
**ESP-IDF Version:** 6.1  
**Board:** ESP32-D (ESP32-D0WD-V3)
