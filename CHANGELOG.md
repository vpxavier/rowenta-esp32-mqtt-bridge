# Changelog

All notable changes to this project are documented in this file.
Format inspired by [Keep a Changelog](https://keepachangelog.com/), versioning follows [Semantic Versioning](https://semver.org/).

---

## [1.0.0] — 2026-08-13

### Added
- Initial public release.
- UART bridge reading the Rowenta mainboard's real-time state (power, mode, LED brightness, air-quality indicator).
- Button control (Power, Mode, Light, Timer) via NPN transistor touch simulation.
- MQTT client with automatic Home Assistant discovery (switch, select, sensors).
- Built-in bilingual (FR/EN) web control interface with auto-refreshing state.
- WiFi/MQTT first-boot configuration portal (captive portal, no hardcoded credentials).
- OTA firmware updates, with a password changeable from the web UI and protected by the current password.
- Double-reset detection to restore the default OTA password if forgotten.
- Non-blocking automatic WiFi reconnection.
- Hardware watchdog (auto-reboot on hang), fed during OTA transfers to avoid interrupting updates.
- Bilingual README, CC BY-NC-SA 4.0 license, wiring diagrams and schematic.

---

*Français ci-dessous / French below*

## [1.0.0] — 2026-08-13

### Ajouté
- Première publication publique.
- Pont UART lisant l'état en temps réel de la carte mère Rowenta (marche/arrêt, mode, intensité LED, indicateur de qualité d'air).
- Pilotage des boutons (Marche, Mode, Lumière, Minuterie) par simulation de contact via transistors NPN.
- Client MQTT avec découverte automatique Home Assistant (switch, select, capteurs).
- Interface web de contrôle embarquée, bilingue (FR/EN), avec actualisation automatique de l'état.
- Portail de configuration WiFi/MQTT au premier démarrage (portail captif, pas d'identifiants codés en dur).
- Mises à jour du firmware par OTA, mot de passe modifiable depuis l'interface web et protégé par le mot de passe actuel.
- Détection de double-reset pour réinitialiser le mot de passe OTA en cas d'oubli.
- Reconnexion WiFi automatique non bloquante.
- Chien de garde matériel (redémarrage automatique en cas de blocage), nourri pendant les transferts OTA pour ne pas interrompre les mises à jour.
- README bilingue, licence CC BY-NC-SA 4.0, schémas de câblage et schéma électrique.
