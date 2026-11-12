# Changelog

All notable changes to this project are documented in this file.
Format inspired by [Keep a Changelog](https://keepachangelog.com/), versioning follows [Semantic Versioning](https://semver.org/).

---

## [1.3.0] — 2026-11-12

### Added
- Firmware update from the browser: a new `/update` page lets you upload a compiled `.bin` file (Arduino IDE: Sketch → Export Compiled Binary), with a progress bar and automatic return to the home page after the restart. It is protected by the OTA password, which is checked before anything is written to flash, and it is also available on the `Rowenta-Setup` portal.
- Restart diagnostics: the settings page now shows the cause of the last restart (power-on / RST button, voltage drop (brownout), watchdog, crash, software restart) and a per-cause counter kept in flash. The setup portal page shows the same information.
- New Home Assistant diagnostic sensors published over MQTT: last restart cause, brownout count, WiFi signal (dBm) and uptime (refreshed every minute).

### Fixed
- The 15-second watchdog timeout was never applied with ESP32 core 3.x: the core already starts the task watchdog, so the firmware's own initialisation failed (`TWDT already initialized` in the boot log) and the core's shorter default timeout stayed active. The watchdog is now reconfigured instead.

## [1.2.2] — 2026-10-11

### Fixed
- The device could stay stuck forever on the `Rowenta-Setup` captive portal even though the WiFi configuration was still saved: if the WiFi network was not reachable within the boot-time window (router still starting, temporary outage), the portal was started and never left. When a saved configuration exists, the device now reboots and retries the connection automatically after 2 minutes of portal inactivity (no client connected to the portal). The saved configuration is never erased.
- The boot-time WiFi connection window was extended from 15 s to 20 s.

## [1.2.1] — 2026-08-14

### Fixed
- Outdoor air quality values were never picked up: the JSON parser matched the `current_units` block (unit labels, e.g. `"pm10":"µg/m³"`) instead of the `current` block (actual numeric values), since both share the same field names and the parser took the first match. Parsing is now restricted to the `current` block. A raw payload log was also added for easier troubleshooting if this ever fails again.

## [1.2.0] — 2026-08-14

### Added
- Outdoor air quality: set your location on an interactive map (Leaflet/OpenStreetMap) on the settings page, and the device fetches outdoor AQI, PM2.5 and PM10 from the free Open-Meteo Air Quality API (no API key required) every 15 minutes, displayed as a new card on the home page.
- "Use my location" button (browser geolocation) as a quick alternative to picking a spot on the map.
- Location is included in the configuration export/import.

## [1.1.1] — 2026-08-14

### Changed
- Added a top navigation menu (Home / Settings) on both pages, replacing the old bottom links.
- The settings page now uses the same dark gradient background and card layout as the home page, for visual consistency.

## [1.1.0] — 2026-08-14

### Added
- Diagnostics section on the settings page: WiFi signal (RSSI), free memory, uptime, MAC address, firmware version.
- Configuration backup/restore: download the current WiFi/MQTT configuration as a `.json` file, and restore it later (or on a replacement device) by pasting it back in, protected by the current OTA password.

## [1.0.2] — 2026-08-14

### Added
- mDNS support: the device is now reachable at `http://rowenta.local` on the local network, no need to look up its IP address.

## [1.0.1] — 2026-08-14

### Added
- UI lockout: all controls are disabled the instant a command is sent, preventing a second command from being fired while the previous one (e.g. a multi-press mode change) is still being processed.
- Automatic page reload after an OTA update: if the web page is open during a firmware update, it detects the device going offline and reloads the home page once it's back online.
- "Author:" label added before the author name in the page footer (bilingual).

### Fixed
- OTA updates no longer get interrupted by the hardware watchdog on larger firmware images: the watchdog is now fed during the OTA transfer itself via the `onProgress` callback.

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

## [1.3.0] — 2026-11-12

### Ajouté
- Mise à jour du firmware depuis le navigateur : une nouvelle page `/update` permet d'envoyer un fichier `.bin` compilé (Arduino IDE : Croquis → Exporter les binaires compilés), avec barre de progression et retour automatique à la page d'accueil après le redémarrage. Elle est protégée par le mot de passe OTA, vérifié avant toute écriture en mémoire flash, et elle est aussi disponible dans le portail `Rowenta-Setup`.
- Diagnostic des redémarrages : la page Paramètres affiche désormais la cause du dernier redémarrage (mise sous tension / bouton RST, chute de tension (brownout), watchdog, plantage, redémarrage logiciel) et un compteur par cause conservé en mémoire flash. La page du portail de configuration affiche les mêmes informations.
- Nouveaux capteurs de diagnostic Home Assistant publiés en MQTT : cause du dernier redémarrage, nombre de chutes de tension, signal WiFi (dBm) et temps de fonctionnement (actualisés chaque minute).

### Corrigé
- Le délai de 15 secondes du watchdog n'était jamais appliqué avec le core ESP32 3.x : le core démarre déjà le watchdog de tâche, donc l'initialisation faite par le firmware échouait (`TWDT already initialized` dans le journal de démarrage) et le délai par défaut du core, plus court, restait actif. Le watchdog est maintenant reconfiguré.

## [1.2.2] — 2026-10-11

### Corrigé
- L'appareil pouvait rester bloqué indéfiniment sur le portail captif `Rowenta-Setup` alors que la configuration WiFi était toujours enregistrée : si le réseau WiFi n'était pas joignable pendant la fenêtre de connexion au démarrage (routeur en cours de démarrage, coupure temporaire), le portail était lancé et jamais quitté. Lorsqu'une configuration est enregistrée, l'appareil redémarre désormais et retente la connexion automatiquement après 2 minutes d'inactivité du portail (aucun client connecté). La configuration enregistrée n'est jamais effacée.
- La fenêtre de connexion WiFi au démarrage passe de 15 s à 20 s.

## [1.2.1] — 2026-08-14

### Corrigé
- La qualité d'air extérieure n'était jamais récupérée : l'analyseur JSON correspondait au bloc `current_units` (les libellés d'unités, ex. `"pm10":"µg/m³"`) au lieu du bloc `current` (les vraies valeurs numériques), les deux blocs partageant les mêmes noms de champs et l'analyseur prenant la première correspondance trouvée. L'extraction est désormais restreinte au bloc `current`. Un journal de la réponse brute a aussi été ajouté pour faciliter le diagnostic en cas de nouveau problème.

## [1.2.0] — 2026-08-14

### Ajouté
- Qualité de l'air extérieure : indiquez votre position sur une carte interactive (Leaflet/OpenStreetMap) depuis la page de paramètres, et l'appareil récupère l'AQI extérieur, les PM2.5 et PM10 via l'API gratuite Open-Meteo Air Quality (sans clé API) toutes les 15 minutes, affiché dans une nouvelle carte sur la page d'accueil.
- Bouton « Utiliser ma position » (géolocalisation du navigateur) en alternative rapide au pointage manuel sur la carte.
- La position est incluse dans l'export/import de configuration.

## [1.1.1] — 2026-08-14

### Modifié
- Ajout d'un menu de navigation en haut de page (Accueil / Paramètres) sur les deux pages, remplaçant les anciens liens en bas de page.
- La page de paramètres reprend maintenant le même fond dégradé sombre et la même disposition en cartes que la page d'accueil, pour une cohérence visuelle.

## [1.1.0] — 2026-08-14

### Ajouté
- Section Diagnostic sur la page de paramètres : signal WiFi (RSSI), mémoire libre, temps de fonctionnement, adresse MAC, version du firmware.
- Sauvegarde/restauration de la configuration : téléchargez la configuration WiFi/MQTT actuelle sous forme de fichier `.json`, et restaurez-la plus tard (ou sur un appareil de remplacement) en la recollant, protégé par le mot de passe OTA actuel.

## [1.0.2] — 2026-08-14

### Ajouté
- Support mDNS : l'appareil est désormais accessible via `http://rowenta.local` sur le réseau local, plus besoin de chercher son adresse IP.

## [1.0.1] — 2026-08-14

### Ajouté
- Verrouillage de l'interface : tous les contrôles sont désactivés dès qu'une commande est envoyée, empêchant qu'une seconde commande parte pendant que la précédente (par exemple un changement de mode à plusieurs appuis) est encore en cours de traitement.
- Rechargement automatique de la page après une mise à jour OTA : si la page web est ouverte pendant une mise à jour, elle détecte la coupure de l'appareil et recharge la page d'accueil une fois celui-ci de nouveau disponible.
- Ajout du label « Auteur : » devant le nom de l'auteur en bas de page (bilingue).

### Corrigé
- Les mises à jour OTA ne sont plus interrompues par le chien de garde matériel sur les images de firmware plus volumineuses : celui-ci est désormais nourri pendant le transfert OTA lui-même, via le callback `onProgress`.

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
