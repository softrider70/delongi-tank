# Copilot Configuration for ESP32 Template

## Skills (Automation)

### Build & Upload
- **/build-project** — Compile the project, generate `${PROJECT_NAME}.bin`
- **/upload** — Smart router (first-time setup vs. update)
- **/upload-firmware** — Fast app-only update (~3 seconds)
- **/initial-upload** — Full bootloader + partition + app (~20 seconds, one-time setup)

### Version Control
- **/commit** — Stage changes, generate smart commit message, push to git

## Supported Boards
The template auto-supports 5 ESP32 variants:
- **ESP32** (dual-core, 520KB SRAM + PSRAM)
- **ESP32-S2** (single-core, 320KB SRAM)
- **ESP32-S3** (dual-core, 512KB SRAM + PSRAM, USB)
- **ESP32-C3** (single-core RISC-V, 400KB SRAM)
- **ESP32-C6** (dual-core RISC-V, 512KB SRAM + PSRAM)

## Workflow
```
1. Code change
2. /build-project       (compile)
3. /upload-firmware     (fast upload)
4. (watch output)
5. /commit              (save to git)
```

## Documentation
- See `README.md` for build, flash, OTA workflow, and operational usage
- See `PROJECT.md` for the current project specification and hardware/runtime details
- See `archive/phase3-reference/README.md` for historical Phase-3 context only

---
Template Version: 0.1.0
