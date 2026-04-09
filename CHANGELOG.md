# Changelog

## 2026-04-09

- Touch-Key auf GPIO 27 (T7) wurde erfolgreich getestet: einmal berühren startet die manuelle Befüllung, ein zweites Mal stoppt sie.

## 2026-04-06

- Branch `master` wurde in `main` umbenannt.
- Remote `origin/master` wurde gelöscht; `origin/main` ist jetzt der primäre Remote-Branch.
- Der lokale Bosch-Fork wurde mit `upstream/main` synchronisiert.
- Funktionale Upstream-Änderung übernommen: Befüllen stoppt beim Erreichen des `OBEN`-Grenzwerts.
- Der `BEFUELLEN`-Button ist jetzt außerdem deaktiviert, wenn der Tank als voll angezeigt wird.
- Touch-Key wurde auf GPIO 27 verschoben, um GPIO 33 für ein Relais frei zu halten.
- OTA-Diagnoseanzeigen wurden im `diagnostics`-Tab ergänzt und werden automatisch aktualisiert.
- OTA-Updateformular wurde hinzugefuegt; lokale Firmware-URLs wie `http://<host-ip>/bosch-tank.bin` sind jetzt als Updatequelle nutzbar.
- Laufzeit- und OTA-Statusdaten werden mittlerweile persistent in NVS gespeichert.
- Build-Nummer und `include/version.h` werden automatisch von `tools/increment_build.py` generiert.
- Ein Backup-Branch `backup-before-upstream-merge` wurde zur Sicherung des vorigen Zustands angelegt.
- `README.md` und `PROJECT.md` wurden zur Dokumentation dieser Projektänderungen aktualisiert.
