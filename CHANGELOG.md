# Changelog

## 2026-04-06

- Branch `master` wurde in `main` umbenannt.
- Remote `origin/master` wurde gelöscht; `origin/main` ist jetzt der primäre Remote-Branch.
- Der lokale Bosch-Fork wurde mit `upstream/main` aus `softrider70/delonghi-tank` synchronisiert.
- Funktionale Upstream-Änderung übernommen: Befüllen stoppt beim Erreichen des `OBEN`-Grenzwerts.
- Der `BEFUELLEN`-Button ist jetzt außerdem deaktiviert, wenn der Tank als voll angezeigt wird.
- OTA-Diagnoseanzeigen wurden im `diagnostics`-Tab ergänzt und werden automatisch aktualisiert.
- Ein Backup-Branch `backup-before-upstream-merge` wurde zur Sicherung des vorigen Zustands angelegt.
- `README.md` und `PROJECT.md` wurden zur Dokumentation dieser Projektänderungen aktualisiert.
