# RFSentinel — Documentation complète du projet

> **Langue :** Français · [English](../../DOCUMENTATION.md)

> Document de référence : vision, architecture, choix techniques, API, utilisation,
> état réel du projet et constantes. Toutes les valeurs sont relevées dans le code
> de la version 1.0.0.

---

## 1. Vision du produit

**RFSentinel** est un capteur autonome de **surveillance passive d'environnement
radio** (Wi-Fi 2.4 GHz + Bluetooth Low Energy) tenu par un dongle **LilyGO
T-Dongle S3 (ESP32-S3)**.

Il :

- **détecte passivement** les équipements à proximité — réseaux Wi-Fi publiés et
  périphériques BLE qui diffusent — sans se connecter à rien ;
- **enregistre** les observations dans sa **flash interne** (LittleFS), avec
  rotation circulaire ;
- fournit une **interface Web embarquée** bilingue FR/EN, thèmes clair et sombre,
  pour consulter, trier, filtrer, cartographier, exporter et purger ;
- **recherche les équipements dans les tables publiques de WiGLE** et permet un
  **upload manuel** du journal au format WiGLE ;
- **alerte en MQTT** à l'apparition d'un nouvel équipement ;
- fonctionne **sans rattachement réseau obligatoire** : un point d'accès de
  secours intégré prend le relais, et les scans ne s'arrêtent jamais ;
- affiche sur son **écran TFT** l'adresse à ouvrir et l'état des scans.

Cas d'usage : wardriving passif, cartographie de réseaux, audit de proximité,
veille d'environnement radio, inventaire BLE, recherche d'un émetteur dans une
pièce.

**Cadre légal et éthique** : RFSentinel n'observe que des trames **librement
diffusées** (beacons Wi-Fi, advertisements BLE). Il ne se connecte à aucun réseau,
ne déchiffre rien, n'intercepte aucune communication et n'identifie personne.
L'utilisateur reste responsable du respect de la réglementation locale sur la
capture de données radio et sur l'usage qu'il en fait.

---

## 2. Cible matérielle

| Élément | Valeur |
|---|---|
| Carte | LilyGO **T-Dongle S3** (ESP32-S3) |
| Flash | **16 Mo interne** — aucune carte SD, la dépendance SD/FatFs a été supprimée |
| PSRAM | **Aucune** — tout est dimensionné en RAM interne |
| Écran | TFT **ST7735**, panneau 128×160 piloté en **160×80 paysage** |
| Wi-Fi | 2,4 GHz uniquement, scan **100 % passif** |
| BLE | NimBLE, **central + observer**, uniquement par advertisements |
| GPS | **Aucun** — latitude/longitude saisies à la main, `0` = inconnu |
| USB | USB-C, périphérique USB-Serial-JTAG |

### Brochage de l'écran

 Relevé dans `display.c`, confirmé sur la carte :

| Rôle | GPIO | Constante |
|---|---|---|
| SPI clock | **5** | `RFS_PIN_LCD_CLK` |
| SPI MOSI | **3** | `RFS_PIN_LCD_MOSI` |
| Data/Command | **2** | `RFS_PIN_LCD_DC` |
| Reset | **1** | `RFS_PIN_LCD_RST` |
| Chip select | **4** | `RFS_PIN_LCD_CS` |
| Rétroéclairage | **38**, **actif à l'état bas** | `RFS_PIN_LCD_BL` |

Le bus est `SPI2_HOST`, cadencé à 20 MHz, mode SPI 0, file de 10 transferts,
16 bits par pixel en ordre BGR. Le format paysage 160×80 de la T-Dongle S3
obtient par `swap_xy(true)` + `mirror(false, true)` + `set_gap(1, 26)`, conforme
à l'exemple officiel LilyGO. Le rétroéclairage s'allume sur `GPIO` bas et
s'éteint sur `GPIO` haut.

---

## 3. Architecture

### 3.1 Organisation

Projet **ESP-IDF v5.3.2** (pas d'Arduino), structure standard :

```
rfsentinel/
├── CMakeLists.txt            # projet (min + idf_project_default)
├── README.md                 # page d'accueil du dépôt
├── DOCUMENTATION.md          # ce fichier
├── LICENSE                   # CC BY-NC-SA 4.0
├── partitions.csv            # nvs 24K, phy_init 4K, factory 3M, storage 13M
├── sdkconfig.defaults        # config de build figée (16 Mo, pas de PSRAM, NimBLE…)
├── logo/                     # SVG (WebUI) + bitmap OLED
│   ├── rfsentinel-logo.svg
│   ├── rfsentinel-icon.svg
│   ├── rfsentinel-oled-icon.svg
│   └── rfsentinel-oled-wordmark.svg
├── screen/                   # captures d'écran de la page d'accueil
├── tools/generate_lcd_logo.py  # SVG -> bitmap 1 bpp pour l'écran
├── tests/                    # tests exécutés sur l'hôte (Python, JS, C)
└── main/
    ├── CMakeLists.txt        # SRCS + EMBED_FILES (ui/index.html + 2 logos)
    ├── idf_component.yml     # dépendances figées (littlefs ==1.23.1)
    ├── version.h             # RFS_VERSION, source unique de la version
    ├── main.c                # bootstrap + boucle d'entretien
    ├── config.c/h            # configuration NVS, secrets, validation
    ├── auth.c/h              # session WebUI (cookie) + anti-force-brute
    ├── net.c/h               # Wi-Fi STA + AP secours + NTP + qualité d'horodatage
    ├── scanner.c/h           # scans passifs Wi-Fi et BLE, calibration, recherche
    ├── store.c/h             # journal LittleFS, CSV WiGLE, rotation, blacklist, export
    ├── mqtt.c/h              # alertes MQTT (esp-mqtt)
    ├── presence.c/h          # MAC surveillées, alertes de présence
    ├── wigle.c/h             # upload et recherche WiGLE (API v2, HTTPS)
    ├── web.c                 # serveur HTTP embarqué (esp_http_server + cJSON)
    ├── display.c/h           # écran ST7735 (boot, statut, veille, langue)
    ├── oled_font.h           # glyphs 8×16 (uint16_t, un mot par colonne)
    ├── oled_logo.h           # logo 64×64 1 bpp (bitmap)
    ├── ui/index.html         # SPA WebUI, embarquée par EMBED_FILES
    └── lcd/esp_lcd_st7735.c/h  # driver ST7735 local
```

### 3.2 Flux de données

```
[Wi-Fi passif]─┐
               ├─► scanner ─► store (CSV /rfs/observations) ─► wigle (upload manuel)
[BLE adv]──────┘      │
                      ├─► mqtt (alertes nouveaux équipements)
                      ├─► presence (équipements surveillés)
                      └─► web (API JSON + UI)  ◄─── navigateur
```

- Le scanner tourne dans sa **propre tâche FreeRTOS** et n'est jamais bloqué par
  le reste ; la pile de la tâche principale est portée à 6144 octets.
- `store` est le **seul** utilisateur du système de fichiers ; `web` ne fait que
  sérialiser en JSON et lire par flux bornés.
- `net` fournit l'heure UTC **au mieux** : NTP si la liaison le permet, sinon
  référence enregistrée + temps de fonctionnement.
- `config` est chargée **une fois** en RAM (`app_config_t`) et écrite en NVS avec
  parcimonie.
- La boucle principale de `main.c` ne fait que `net_tick()` et `display_tick()`
  chaque seconde, avec un rafraîchissement d'écran tous les 5 s.

### 3.3 Principes de conception

1. **Allocation dynamique bornée** : tampons statiques pour le chemin critique,
   tas réservé aux requêtes ponctuelles, et libérations sur toutes les sorties.
2. **Jamais bloquant** dans la boucle principale : scans périodiques, tick réseau
   non bloquant, upload et purge exécutés en arrière-plan avec statut intermédiaire.
3. **Résilience aux coupures** : LittleFS, journal en fichiers, rotation douce.
   Rien ne se détruit au redémarrage, la config survit en NVS.
   `format_if_mount_failed=false` : un échec de montage ne déclenche à lui seul
   aucune mise en forme. La partition `storage` est relue intégralement et LittleFS
   n'est initialisé que si chaque octet vaut `0xFF`, c'est-à-dire une carte jamais
   utilisée. Une erreur de lecture, un octet programmé, une partition chiffrée ou
   absente signifient une erreur retournée et rien n'est effacé.
4. **Écritures flash limitées** : NVS réservé à la config, référence d'horloge
   écrite au plus une fois par heure, journal en segments de 32 Ko.
5. **Dépendances minimales** : ESP-IDF + `joltwallet/littlefs` (composant managé,
   version figée) + cJSON + NimBLE + esp_http_server + esp_http_client + esp-mqtt.
6. **Secrets jamais exposés** : token WiGLE, mots de passe et clé Wi-Fi ne
   sortent ni des réponses API, ni des journaux, ni des exports.
7. **Une fonctionnalité n'est pas opérationnelle parce qu'elle compile** : elle
   l'est quand elle a été observée sur la carte.

---

## 4. Choix techniques et justifications

| # | Choix | Alternative écartée | Pourquoi |
|---|---|---|---|
| 1 | **ESP-IDF v5.3.2** | Arduino / PlatformIO | contrôle fin de httpd, NimBLE, NVS et esp_lcd ; build reproductible |
| 2 | **LittleFS** (`joltwallet/littlefs` ==1.23.1) | SPIFFS | usure répartie, dossiers, suppressions propres ; version figée = builds reproductibles |
| 3 | **Flash interne, pas de SD** | carte SD FatFs | un retrait de carte ne détruit plus le journal ; pas de connecteur, pas de bourrage |
| 4 | **Point de montage `/rfs`** | `/sd` | plus de carte SD ; chemin unique et explicite |
| 5 | **Rotation par fichiers et segments** | effacement global | on ne perd jamais tout ; les plus anciens partent un par un |
| 6 | **Segments de 32 Ko dans le fichier du jour** | un fichier par jour sans borne | borne la quantité d'historique que la rotation peut emporter |
| 7 | **Scan Wi-Fi 100 % passif** (`WIFI_SCAN_TYPE_PASSIVE`) | scan actif | furtivité, légalité, aucune association, aucun trafic émis |
| 8 | **BLE par advertisements seulement** | connexion GATT | observer sans interagir ; `filter_duplicates=1` |
| 9 | **NimBLE central + observer** | observer seul | la recherche d'un émetteur proche a besoin du rôle central ; peripheral et broadcaster restent désactivés |
| 10 | **CSV `WigleWifi-1.6`** | format propriétaire | le journal se recharge tel quel sur WiGLE, sans conversion |
| 11 | **Upload WiGLE manuel** | upload automatique | contrôle de l'utilisateur, quota, éthique |
| 12 | **Recherche WiGLE côté capteur** | appel direct depuis le navigateur | le token ne quitte jamais l'appareil |
| 13 | **Position saisie à la main + géocodage** | GPS embarqué | le dongle n'a pas de GPS ; la recherche d'adresse évite la saisie de coordonnées |
| 14 | **Carte en `<canvas>` maison + tuiles OSM** | Leaflet | aucune librairie externe à embarquer, repli sur une grille sans Internet |
| 15 | **Deux seuils RSSI distincts** (`min_rssi`, `mqtt_min_rssi`) | seuil unique | un équipement lointain peut être enregistré sans déclencher d'alerte |
| 16 | **STA + AP de secours** | STA seul | utilisable hors réseau ; l'AP s'active si la STA échoue, **sans interrompre les scans** |
| 17 | **NTP + référence enregistrée** | horodatage brut | horodatage exploitable même hors ligne, avec qualité `ESTIMATED` assumée |
| 18 | **Heure réglée depuis le navigateur** | aucun repli hors ligne | en mode AP seul, aucune source d'heure n'est joignable ; la qualité reste `ESTIMATED`, sans usurper une synchro NTP |
| 19 | **NVS pour la config** | fichier de config | atomicité, robustesse, aucune dépendance au système de fichiers |
| 20 | **UI embarquée** (`EMBED_FILES`) | CDN externe | fonctionne en mode AP sans Internet |
| 21 | **Bilingue FR/EN + thèmes clair et sombre** | mono-langue, thème unique | l'appareil se consulte aussi bien en extérieur qu'en intérieur |
| 22 | **Session cookie + anti-force-brute** | basic auth seule | l'appareil ne doit pas être pilotable par n'importe qui sur le réseau |
| 23 | **Changement de mot de passe imposé si `admin/admin`** | valeurs par défaut acceptées | hygiène de base |
| 24 | **Ressources bitmap préconverties** | parsing SVG sur MCU | un ESP32 ne parse pas du SVG ; police 8×16 et logo 64×64 en 1 bpp dans la flash |
| 25 | **WiGLE en HTTPS (bundle CA complet)** | HTTP | le token ne circule jamais en clair |
| 26 | **Déduplication + blacklist** | enregistrement brut | éviter le spam de répétitions et ignorer définitivement des équipements connus |
| 27 | **Tampons WiGLE réduits, lignes plafonnées** | copier toute la réponse | sans PSRAM, le tas est la ressource rare : 6 Ko de corps, 10 lignes maximum |

---

## 5. Modèle de données

### 5.1 Observation (`s3_obs_t`, `store.h`)

```c
typedef struct {
    char mac[18];     // "AA:BB:CC:DD:EE:FF" (BSSID Wi-Fi ou BDADDR BLE)
    char ssid[33];    // SSID Wi-Fi ou nom BLE (peut être vide)
    char auth[16];    // Wi-Fi : "Open", "WPA2"… / BLE : type d'adresse
    char ts[20];      // "YYYY-MM-DD HH:MM:SS" en UTC
    int  channel;     // canal Wi-Fi (0 pour BLE)
    int  freq;        // kHz (2412..2472 en Wi-Fi, 0 pour BLE)
    int  rssi;        // dBm (négatif)
    int  mfgr;        // réservé (0)
    bool is_ble;      // false = Wi-Fi, true = BLE
    uint8_t time_quality;  // 0 inconnu, 1 estimé, 2 synchronisé NTP
} s3_obs_t;
```

### 5.2 Écriture dans le journal

Une ligne par observation, dans l'ordre des champs du format WiGLE : MAC, SSID
(échappé), verrouillage, heure UTC, canal, radio (24 GHz), force, latitude et
longitude configurées, fabricant, puis le type `WIFI` ou `BLE` en dernière
colonne. Les champs que le capteur ne peut pas connaître restent vides.

Pour le BLE, le champ `auth` sert à noter le **type d'adresse** : `BLE-PUBLIC`,
`BLE-STATIC`, `BLE-RPA`, `BLE-NRPA` ou `BLE-UNKNOWN`. Il s'écrit dans la colonne
`RCOIs` du fichier journal, jamais dans la colonne `Type`, qui garde `BLE` et continue
de se lire comme un CSV WiGLE standard. Ce marqueur interne est retiré à l'export
(§5.3) : le fichier téléchargé ne présente que des champs WiGLE.

Le type vient de `main/ble_address.h`, qui range chaque adresse rencontrée en
`PUBLIC`, `STATIC`, `RPA`, `NRPA` ou `UNKNOWN`. Les adresses arrivent en poids
faible d'abord ; un type NimBLE 0 ou 2 — adresse publique, ou identité publique
résolue — donne `PUBLIC`. Pour un type 1, adresse aléatoire, les deux bits de
poids fort de `addr[5]` décident : `0xc0` statique, `0x40` RPA, `0x00` NRPA. Un
type 3 dont ces bits n'annoncent pas une adresse statique, ou un type non
reconnu, tombe en `UNKNOWN`.

### 5.3 Journal (`store.c`)

- Un fichier **par jour** : `/rfs/observations/obs_YYYYMMDD.csv`, avec l'en-tête
  WiGLE en première ligne.
- Le fichier du jour est découpé en **segments de 32 Ko**
  (`RFS_SEGMENT_MAX_BYTES`) : une rotation n'emporte jamais plus d'un segment,
  donc au plus 32 Ko d'histoire.
- Un nouveau segment s'ouvre aussi quand la **qualité d'horodatage change**, pour
  que des heures sûres et des heures estimées ne se mélangent pas dans un segment.
- **Rotation circulaire** : au-delà de `RFS_USAGE_LIMIT_PCT` (90 % de la
  partition), les fichiers les plus anciens partent **un par un** ; un plafond de
  `RFS_OBS_MAX_FILES` (32) fichiers borne aussi la croissance. Jamais de
  formatage total, et **les scans ne s'interrompent jamais** parce que le journal
  est plein.
- **Déduplication** : table `s_seen` de 128 MAC en RAM ; un équipement redevient
  « nouveau » après `rediscover_min` minutes d'absence.
- **Blacklist** : MAC ignorées à l'écriture, ajoutées et retirées depuis l'UI,
  persistées en NVS (`bl_` plus un index `bl_idx`), cache de 64 entrées.
- **Tri et filtres à la demande** : clés `date`, `mac`, `ssid`, `rssi`, `tsq`,
  `type`, en sens croissant ou décroissant, avec filtre par plage horaire.
- **Export** : lecture séquentielle de tous les segments dans un seul flux CSV au
  format WiGLE, sans jamais charger le journal en mémoire.
  Le filtre sur les adresses BLE (§5.2) s'applique à la lecture : les lignes dont
  le type d'adresse est exclu par le réglage ne sortent pas dans le fichier, et le
  marqueur interne écrit en colonne `RCOIs` est retiré, pour que l'export ne
  présente que des champs WiGLE. Une ligne BLE enregistrée avant l'apparition du
  marqueur a un type inconnu : elle passe en mode « Toutes » et disparaît dans les
  deux autres.
- **Longueur de l'export** : la réponse HTTP est un `multipart` dont la longueur
  doit être annoncée avant le premier octet. `store_export_open()` parcourt donc
  une première fois le journal filtré pour compter les octets, puis remet le
  curseur à zéro. Les deux passes lisent la même copie figée du journal ; si une
  rotation intervient entre elles, la lecture s'arrête sur
  `ESP_ERR_INVALID_STATE` plutôt que de servir un fichier dont la longueur ne
  correspondrait pas à l'en-tête.
- **Premier montage sur une carte neuve** : `store_init()` monte `storage` et, en
  cas d'échec, appelle `store_partition_is_blank()`, qui parcourt toute la partition
  par blocs de 1 Ko — avec une pause tous les 64 Ko pour ne pas affamer la boucle
  principale. Seule une partition entièrement à `0xFF`, signature d'une carte jamais
  écrite, est formatée puis montée. Tout le reste conserve le journal tel quel et
  signale le stockage indisponible, ce que l'interface affiche au lieu d'une liste
  vide.

### 5.4 Configuration (`config.h`, en NVS)

`app_config_t` regroupe : Wi-Fi (SSID et clé), WebUI (utilisateur et mot de
passe), WiGLE (nom d'API et token), AP de secours (SSID et mot de passe),
latitude/longitude, `min_rssi`, `mqtt_min_rssi`, `rediscover_min`,
`scan_wifi`, `scan_ble`, `ble_address_filter`, `scan_autostart`, `ui_lang`,
MQTT (activation, broker, port, utilisateur, mot de passe, sujet, identifiant,
TLS), NTP (serveur et fuseau), `device_id` et le nom réseau `hostname`.

Règles :

- `hostname` est conservé en NVS et vaut `rfsentinel` par défaut. Il contient
  1 à 32 lettres ASCII minuscules, chiffres ou tirets, et commence et se termine
  par une lettre ou un chiffre. Après redémarrage, il est utilisé pour le nom
  DHCP et le nom mDNS ; `.local` est ajouté seulement pour l'accès mDNS. Une
  valeur enregistrée invalide revient au nom par défaut.
- `config_needs_password_change()` renvoie vrai tant que les identifiants valent
  `admin`/`admin` ; l'interface force alors le changement.
- `config_has_password()`, `config_has_token()` et `config_has_ap_password()` ne
  renvoient que des booléens, **jamais la valeur**.
- Le token WiGLE n'est jamais relu par l'interface après enregistrement : six
  champs sensibles affichent huit points, et un œil permet de les remplacer, jamais
  de les retrouver.
- `config_save_time_ref()` limite l'écriture de la référence d'horloge à une fois
  par heure pour ménager la flash.
- `ble_address_filter` (clé NVS `ble_filter`, octet non signé, défaut « stables »)
  décide quelles adresses BLE le journal garde et lesquelles l'export laisse passer
  : 0 toutes, 1 publiques et aléatoires statiques, 2 publiques seules. Une valeur
  hors plage relue en NVS revient au défaut au chargement, et
  `config_set_ble_address_filter()` refuse ce qui dépasse 2.

---

## 6. API Web (`web.c`)

Serveur `esp_http_server`, réponses JSON via cJSON. `httpd_start` retourne un
`esp_err_t` **vérifié**, et `max_uri_handlers` est dimensionné à 29 pour les 28
gestionnaires répartis sur 27 routes.

Toutes les routes exigent une session, sauf `/api/login`. La session est un cookie
`sid` en `HttpOnly`, `SameSite=Strict`, `Path=/`, `Max-Age=3600`. Trois échecs
d'affilée — par la route de connexion ou par le repli HTTP Basic — provoquent une
temporisation de 30 s.

| Méthode | Route | Rôle |
|---|---|---|
| GET | `/` | interface Web (HTML embarqué) |
| GET | `/logo/rfsentinel-logo.svg` | logo (embarqué) |
| GET | `/logo/rfsentinel-icon.svg` | icône (embarquée) |
| POST | `/api/login` | connexion `{user, pass}` → cookie de session |
| POST | `/api/logout` | déconnexion |
| GET | `/api/config` | lecture de la config, secrets masqués |
| POST | `/api/config` | mise à jour par fusion partielle, validation interne |
| GET | `/api/scan/status` | mode, IP, heure et qualité, version, compteurs, occupation du journal, état MQTT, diagnostics de scan |
| POST | `/api/scan/start` | démarrer les scans |
| POST | `/api/scan/stop` | arrêter les scans |
| GET | `/api/observations` | liste paginée — `page`, `type`, `sort`, `dir`, `cursor`, `from`, `to` |
| GET | `/api/history` | 10 apparitions locales les plus récentes d'une MAC (`mac`) |
| GET | `/api/presence` | appareils surveillés, sujet MQTT et durée d'absence |
| POST | `/api/presence` | ajouter ou retirer un appareil surveillé (`action`, `mac`, `label`) |
| GET | `/api/blacklist` | liste des exclusions |
| POST | `/api/blacklist/add` | ajouter une exclusion `{mac, label}` |
| POST | `/api/blacklist/remove` | retirer une exclusion `{mac}` |
| POST | `/api/wigle/upload` | déclencher l'upload manuel du journal vers WiGLE |
| GET | `/api/wigle/upload/status` | progression et résultat de l'upload |
| GET | `/api/wigle` | ce que WiGLE connaît d'une MAC (`mac`, `type`) |
| POST | `/api/purge` | purge par plage de dates `{from, to}` |
| GET | `/api/purge/status` | progression de la purge |
| GET | `/api/calibration` | état de la mesure en cours |
| POST | `/api/calibration` | `start`, `stop`, `apply`, `find_start`, `find_next` |
| GET | `/api/calibration/find` | candidats BLE repérés pendant la recherche |
| GET | `/api/export` | export CSV du journal (`from`, `to`) en pièce jointe |
| GET | `/api/time` | heure courante et qualité d'horodatage |
| POST | `/api/time` | réglage de l'horloge depuis l'heure du navigateur |

Détails utiles :

- `/api/observations` renvoie 14 lignes par page ; un `type` autre que `wifi` ou
  `ble` est refusé par `bad_type`.
- `/api/config` accepte `ble_address_filter` (0, 1 ou 2) et le renvoie en lecture.
  Une valeur non entière, négative ou supérieure à 2 répond `400` avec
  `ble_filter_invalid` ; un échec d'enregistrement en NVS répond `500` avec
  `ble_filter_save_failed`. Dans les deux cas le réglage précédent reste en place.
- `/api/scan/status` ajoute cinq compteurs de diagnostic BLE — `ble_public`,
  `ble_static`, `ble_rpa`, `ble_nrpa`, `ble_unknown` — qui comptent les adresses
  rencontrées depuis le démarrage, **avant** filtrage : ils font donc voir le
  volume d'adresses temporaires que le réglage écarte du journal.
- `/api/export` exige `from` et `to` en epoch, refuse les bornes invalides ou
  inversées, et répond en `text/csv` avec `Content-Disposition: attachment` et
  `Cache-Control: no-store`.
- `/api/scan/status` expose aussi les tentatives et échecs de scan Wi-Fi, le canal
  exploré et l'âge du dernier cycle : c'est ce qui a permis de diagnostiquer un
  scan muet depuis l'interface.
- Les routes en POST sont déclarées en `HTTP_POST`. Les corps sont lus avec un
  plafond de 4096 octets.
- `/api/presence` est la seule route déclarée deux fois, en GET et en POST,
  d'où 28 gestionnaires pour 27 chemins distincts.
- L'upload travaille en arrière-plan sur tous les fichiers du journal et agrège les
  résultats ; l'interface suit l'avancement par `/api/wigle/upload/status`.

---

## 7. WebUI (`ui/index.html`)

SPA mono-fichier embarquée par `EMBED_FILES`, sans aucune dépendance JavaScript
externe. Bilingue FR/EN avec persistance locale du choix, **thèmes clair et
sombre** basculés par bouton et mémorisés en `localStorage` sous la clé
`rfs-theme` ; le thème sombre est celui par défaut.

Vues et cartes :

- **Connexion** — identifiants, changement de mot de passe imposé au premier
  lancement, message explicite pendant la temporisation anti-force-brute.
- **État** — mode réseau, IP, version, heure locale avec l'UTC en infobulle,
  dernière synchronisation, compteurs Wi-Fi et BLE réellement lus dans le journal,
  ligne de répartition des adresses BLE depuis le démarrage — publiques,
  statiques, RPA, NRPA et inconnues —, occupation du stockage, état MQTT,
  diagnostics de scan, bouton de réglage de l'heure.
- **Observations** — liste paginée de 14 lignes, tri par colonne, filtre Wi-Fi/BLE,
  filtre par plage de dates en heure locale, ligne cliquable.
- **Historique de l'équipement** — présences connues de WiGLE pour la MAC cliquée,
  dans l'ordre chronologique, avec un point sur la carte à droite.
- **Carte** — `<canvas>` dessinée à la main : quadrillage de secours, puis
  assemblage de vraies tuiles OpenStreetMap
  (`https://tile.openstreetmap.org/{z}/{x}/{y}.png`, 256 px) quand le poste a
  accès à Internet. Zoom calculé depuis l'étalement réel du nuage de points et
  borné entre 6 et 17 ; projection relative aux bornes du nuage, correction de la
  longitude selon la latitude, cercle de précision autour du point sélectionné,
  recalage sur la taille affichée et la densité de l'écran.
- **Configuration** — Wi-Fi, AP de secours, position, seuils, options de scan,
  langue, WiGLE, MQTT, NTP, fuseau et identifiant d'appareil. Un sélecteur
  « Adresses BLE » choisit le filtre appliqué au journal et aux exports —
  Toutes, Stables par défaut, Publiques uniquement —, avec une ligne d'aide qui
  rappelle que les anciennes lignes BLE sans type restent exclues des exports
  filtrés. Les six champs sensibles sont masqués, avec un œil pour les remplacer.
- **Géocodage** — recherche d'adresse vers la base adresse nationale
  (`https://data.geopf.fr/geocodage/search`) pour remplir latitude et longitude.
- **Export** — téléchargement de tout le journal ou d'une plage, en CSV WiGLE prêt
  à renvoyer sur wigle.net.
- **Purge** — suppression par plage de dates avec confirmation explicite et suivi
  de progression ; rien n'est effacé à l'aveugle.
- **Calibration BLE** — mesure de force d'un équipement à trois positions pour en
  déduire un seuil, puis application du seuil obtenu ; recherche d'un émetteur
  proche par comparaison de trois relevés, avec liste de candidats.
- **Équipements exclus** — blacklist avec libellé.
- **Alertes de présence** — adresses MAC surveillées avec libellé, sujet MQTT
  et durée d'absence réglables.
- **Upload WiGLE** — bouton, confirmation, statut intermédiaire, résultat ;
  déconnexion accessible dans le bandeau.

La carte ne charge **aucune** librairie de cartographie : tout le rendu est local,
ce qui la fait fonctionner même en mode AP sans Internet, sur la grille de secours.

---

## 8. MQTT (`mqtt.c`)

- `esp-mqtt` ; broker, port, utilisateur, mot de passe, sujet, identifiant client
  et TLS configurables. Démarré au lancement, pas seulement après un enregistrement
  de configuration.
- `mqtt_on_obs(obs)` est appelée par le scanner **après enregistrement**. Elle
  filtre sur `mqtt_min_rssi`, applique la déduplication avec `rediscover_min`,
  puis publie en **QoS 1, sans retain**.
- Vérification et publication se font dans la même section critique, parce que les
  rappels BLE et la tâche Wi-Fi arrivent en parallèle.
- Charge utile, plafonnée à 384 octets :

```json
{"device_id":"…","mac":"…","type":"WIFI","ssid":"…","rssi":-72,
 "ts":"2026-11-11 10:50:10","tsq":"NTP_SYNCED","lat":48.85660,"lon":2.35222}
```

`tsq` donne la qualité de l'horodatage et prend trois valeurs : `NTP_SYNCED`,
`ESTIMATED` — l'heure repart d'une référence antérieure, sans revalidation — et
`UNKNOWN` quand l'appareil n'a aucune idée de l'heure, `ts` valant alors `"0"`. Un
consommateur peut ainsi écarter les messages mal horodatés.

---

### 8.1 Équipements surveillés (`presence.c`)

`presence.c` tient une liste de 32 adresses MAC et leur libellé, persistée dans un
espace NVS qui lui est propre (`rfs_presence`). La surveillance est indépendante
des seuils d'enregistrement et de la liste des exclusions :

- côté Wi-Fi, `presence_on_obs()` est appelée **avant** le test d'exclusion et
  avant tout filtrage sur le signal ;
- côté BLE, `presence_matches()` est consultée avant les filtres, sans quoi un
  appareil surveillé serait écarté sur un signal faible ; l'observation est alors
  transmise même si elle n'est pas journalisée ;
- le premier relevé déclenche une alerte quel que soit le niveau de signal ; une
  alerte de retour n'est émise qu'après une absence d'au moins `absence_s`
  (120 s par défaut, 30 s minimum), avec nouvelle tentative au plus toutes les 5 s ;
- `mqtt_presence_publish()` publie en QoS 1 sur un sujet dédié, jamais le sujet des
  alertes : la configuration refuse que les deux soient identiques.

Charge utile, plafonnée à 512 octets :

```json
{"event":"presence","device_id":"…","mac":"…","type":"BLE","label":"…",
 "ssid":"…","rssi":-71,"ts":"2026-11-11 10:50:10","tsq":"NTP_SYNCED"}
```

`label` reprend le libellé saisi dans l'interface, pour qu'un consommateur
distingue plusieurs appareils surveillés sans connaître les adresses MAC.

---

### 8.2 Interfaçage avec Home Assistant

RFSentinel se comporte comme un client MQTT ordinaire : il **publie**, il n'écoute
rien et n'expose aucun point d'entrée. Un domotique connecté au même broker —
Home Assistant par son intégration MQTT, Node-RED, openHAB ou un script — **écoute**
le topic qui l'intéresse et décide de ce qu'il en fait. Rien n'est spécifique à
RFSentinel côté consommateur.

| Topic par défaut | Déclencheur | Champs utiles |
|---|---|---|
| `/rfsentinel/alert` | nouvel équipement journalisé | `type`, `mac`, `ssid`, `rssi`, `ts`, `tsq`, `lat`, `lon` |
| `/rfsentinel/presence` | apparition ou retour d'un équipement surveillé | `event`, `label`, `type`, `mac`, `ssid`, `rssi`, `ts`, `tsq` |

Les deux sujets se renommement dans la page de configuration et ne peuvent pas
porter le même nom (§8.1). Les deux publications sont en QoS 1, sans retain : un
consommateur éteint ne reçoit pas rattrapage à sa reconnexion.

Dans Home Assistant, l'automatisation se construit sur le déclencheur
**« Lorsqu'un message MQTT a été reçu »**, avec le topic à écouter. L'action est
ensuite libre : notification persistante, envoi vers un téléphone, allumage d'une
lumière, message vocal. Les champs du JSON se relisent dans un modèle via
`trigger.payload_json` :

```yaml
{{ trigger.payload_json.type }} détecté — SSID : {{ trigger.payload_json.ssid or 'inconnu' }} — MAC : {{ trigger.payload_json.mac }} — RSSI : {{ trigger.payload_json.rssi }} dBm
```

Deux champs appellent une condition explicite dans l'automatisation.

`rssi` se comporte différemment selon le topic. Sur `/rfsentinel/alert` c'est
toujours un nombre, mais un équipement BLE y figure avec 127 lorsque le signal n'a
pas été mesuré : un seuil du type « RSSI supérieur à -80 » écarterait donc ces
équipements par accident. Sur `/rfsentinel/presence` le même cas s'écrit `null`,
et un modèle qui affiche le champ sans filtre produit alors « dBm » sans nombre.

`tsq` vaut `NTP_SYNCED`, `ESTIMATED` ou `UNKNOWN`, et permet d'écarter les
messages émis avant synchronisation de l'heure : dans ce dernier cas `ts` vaut
`"0"`, ce qui fausse une automatisation fondée sur l'heure au premier démarrage.

Le topic de présence ne parle que quand un équipement surveillé apparaît ou
réapparaît : après une absence d'au moins `absence_s`, son retour déclenche une
nouvelle publication, et rien n'est émis pendant l'absence (§8.1). Il n'existe pas
de message de départ. Une automatisation qui doit réagir à une disparition —
éteindre une lumière quand le téléphone quitte le logement — ne peut donc pas
l'attendre de RFSentinel : elle allume sur le message reçu et s'éteint elle-même
après un délai, `for:` en anglais, supérieur à la période de redetection radio.

---

## 9. WiGLE (`wigle.c`)

Cinq points d'entrée de l'API v2 sont utilisés, tous en **HTTPS** avec le bundle CA
complet et une authentification **Basic** (nom d'API + token) :

| Usage | URL |
|---|---|
| Upload | `https://api.wigle.net/api/v2/file/upload` |
| Recherche Wi-Fi | `https://api.wigle.net/api/v2/network/search` |
| Recherche Bluetooth | `https://api.wigle.net/api/v2/bluetooth/search` |
| Détail Wi-Fi | `https://api.wigle.net/api/v2/network/detail` |
| Détail Bluetooth | `https://api.wigle.net/api/v2/bluetooth/detail` |

Points vérifiés en direct contre l'API, après des échecs répétés :

- le paramètre d'adresse s'appelle **`netid`** dans les deux tables ; `mac`,
  `bdaddr` et `bssid` n'existent pas et provoquent des erreurs 400 ;
- le nombre de lignes se demande avec **`resultsPerPage`** ;
- la position d'une recherche simple est **`trilat` / `trilong`**, et non `lat` /
  `lon` — une recherche qui aboutissait laissait la carte vide ;
- les positions datées viennent des points d'entrée **`detail`** :
  `network/detail` renvoie `locationData[]` (`latitude`, `longitude`, `time`,
  `signal`, `accuracy`) et `bluetooth/detail` renvoie
  `locationClusters[].locations[]` avec les mêmes champs ; sans point détaillé, la
  position trilatérale d'en-tête est reprise avec la période couverte ;
- un `401` est traduit en `RFS_ERR_WIGLE_AUTH` pour que l'interface parle
  d'identifiants refusés plutôt que d'équipement introuvable.

Contraintes mémoire, imposées par l'absence de PSRAM : tampon de corps de 6 Ko, 32
lignes maximum, tableau de lignes alloué une seule fois pour toute la recherche au
lieu d'un bloc par requête, et libérations sur toutes les sorties anticipées. Le
tas libre est journalisé avant et après une recherche pour permettre un diagnostic
sur la carte.

Ces précautions ne suffisent pas tant que l'écran est allumé : le framebuffer du
panneau occupe le plus gros bloc de RAM interne et bloque l'ouverture de la session
TLS. Le plus gros bloc disponible n'est rendu que par l'extinction automatique, une
minute après la mise sous tension (§11.5). Une recherche WiGLE lancée avant ce
moment peut échouer par manque de mémoire : la recherche s'arrête sur
`ESP_ERR_NO_MEM` et répond `error:nomem`, ce qui se distingue d'une adresse
absente chez WiGLE.

L'upload se fait en POST multipart avec une boundary dédiée, **uniquement** sur clic
de l'utilisateur. Le nom d'API et le token ne sortent jamais de l'appareil.

---

## 10. Réseau et horloge (`net.c`)

### 10.1 Deux modes, un seul objectif : ne jamais perdre de données

L'appareil tente d'abord de rejoindre le réseau Wi-Fi enregistré. Cet essai est
borné : `STA_CONNECT_TIMEOUT_MS` (8 s) par tentative, `STA_MAX_RETRY` (4)
tentatives au total. Au-delà, il bascule en point d'accès `RFSentinel-Setup` (mot
de passe `012345678`, interface `192.168.4.1`) et l'interface reste joignable
depuis un téléphone ou un ordinateur.

Le point important est que le basculement n'interrompt pas la collecte : les scans
continuent et le journal s'écrit aussi bien en point d'accès qu'en mode client. Un
équipement posé dans un lieu sans réseau disponible remplit donc son journal,
qu'il sera possible de consulter ou de faire remonter plus tard.

### 10.2 Boucle non bloquante

`net_tick()` est appelée régulièrement par la boucle principale. Elle avance l'état
du réseau par petits pas — une tentative de connexion, une vérification de délai,
une requête NTP — sans jamais bloquer. Une connexion Wi-Fi qui traîne ou un
serveur NTP injoignable ne suspend donc ni l'écran, ni l'interface, ni les scans.

### 10.3 L'horloge et sa qualité

Un horodatage sans référence de temps ne vaut rien pour une cartographie. L'appareil
connaît donc trois états, enregistrés à chaque observation :

| Qualité | Signification | Source |
|---|---|---|
| `TS_UNKNOWN` | temps non réglé | aucune source exploitable |
| `TS_ESTIMATED` | temps reconstruit | `FALLBACK_EPOCH` (2025-01-01 UTC) + durée de marche |
| `TS_NTP_SYNCED` | temps vérifié | serveur NTP |

Le troisième état n'est jamais simulé. Si l'heure est réglée depuis le navigateur —
route `POST /api/time`, qui transmet l'heure locale et le décalage — la qualité
passe à `TS_ESTIMATED` et non à `TS_NTP_SYNCED` : l'information vient du poste de
l'utilisateur, pas d'une source de temps reconnue. Cette distinction se retrouve
dans l'export, où le champ `comment` signale les observations à temps estimé.

Un réglage manuel est conservé en mémoire non volatile toutes les
`TIME_REF_SAVE_INTERVAL_S` (3 600 s), ce qui permet de retrouver une heure cohérente
après un redémarrage sans réseau. Le serveur NTP par défaut est `pool.ntp.org` et
le fuseau `CET-1CEST,M3.5.0,M10.5.0/3`, tous deux modifiables. Une
resynchronisation est tentée toutes les `NTP_RESYNC_INTERVAL_S` (6 h).

### 10.4 Affichage de l'heure

L'heure réglée est stockée en UTC. Les affichages de l'interface — dernière
synchronisation, heures des observations — passent par `net_time_local` et
`net_last_sync_local`, qui appliquent le fuseau configuré. Une conversion faite
avec `gmtime_r` affichait une heure décalée d'une heure par rapport à l'heure
locale réelle ; le journal, l'export CSV et les messages MQTT restent, eux, en UTC,
ce qui est le comportement attendu pour des données destinées à être traitées
ailleurs.

---

## 11. Écran (`display.c`)

### 11.1 Contrainte matérielle

L'écran fait 160 × 80 pixels et n'affiche qu'une couleur effective, mais le
contrôleur ST7735 attend des pixels RV565 : le framebuffer fait donc
`160 × 80 × 2 = 25 600 octets`. Il est alloué par `heap_caps_malloc()` avec les
drapeaux `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`, parce que le transfert SPI
exige une mémoire accessible au DMA et que la carte n'a pas de PSRAM. C'est le
plus gros bloc de RAM interne consommé par l'appareil, et il est libéré dès que
l'écran s'éteint — voir §11.5.

### 11.2 Rotation logicielle

Le contrôleur de la carte est configuré avec `swap_xy(true)`, `mirror(false, true)`
et un décalage de 26 pixels en X et 1 en Y, ce qui présente l'image tournée de 180°
par rapport au repère utilisé par le dessin. `rotate_frame()` retourne le
framebuffer avant l'envoi. Conséquence pratique : les coordonnées utilisées par le
code de dessin sont bien les coordonnées visibles à l'écran.

Cette rotation a été ajoutée après un retour matériel — l'image était tête en bas —
et la correction a été confirmée sur la carte.

### 11.3 Écran d'accueil

Le logo, 64 × 64, est collé en haut de l'écran (`y0 = 0`). Le bas reçoit la
signature `By M3r0W1g`, centrée sous le logo. Les deux tiennent dans la
hauteur disponible : 64 pixels de logo plus 16 pixels de ligne de texte = 80
pixels, sans chevauchement ni rognure.

### 11.4 Écran de statut

Après l'accueil, l'écran affiche en continu :

- `RFSentinel v1.0.0` ;
- l'adresse IP obtenue, ou `Connexion...` tant que le réseau n'est pas prêt ;
- `Mode: Wi-Fi` ou `Mode: AP` selon le mode réseau actif.

L'accueil revient au démarrage et pendant `RFS_BOOT_HOLD_MS` (60 s), puis la
veille prend le relais pour limiter l'échauffement et la consommation. Le
rétroéclairage est commandé par une broche active à l'état bas.

### 11.5 Extinction de l'écran et mémoire récupérée

`display_tick()` compare l'horloge au démarrage et appelle `display_blank()`
dès que `RFS_BOOT_HOLD_MS` est écoulé, soit **une minute** après la mise sous
tension. `display_blank()` coupe le panneau (`disp_on_off(false)`), relève la
broche de rétroéclairage, puis `release_frame()` libère le framebuffer et remet
le pointeur à `NULL`. La libération n'intervient qu'après la fin du transfert
SPI en cours, pour ne jamais rendre un tampon pendant une écriture DMA.

Ce comportement n'est pas qu'un détail de confort : les 25 600 octets rendus au
tas interne sont précisément ce qui permet à `wigle.c` d'ouvrir une session TLS
et de recevoir un corps de réponse. Tant que l'écran reste allumé, la recherche
WiGLE peut manquer de mémoire contiguë. **Il faut donc attendre l'extinction de
l'écran avant de consulter l'historique WiGLE d'un équipement.** Le journal série
trace les deux moments utiles au diagnostic : `tampon ecran libere: libre=…
bloc_max=…` côté écran, et le tas libre avant/après chaque appel côté WiGLE.

Une fois éteint, l'écran ne se rallume pas : les écrans d'accueil et de statut
sortent immédiatement si `s_blanked` est vrai. L'appareil revient à l'affichage
au redémarrage suivant.

Les libellés suivent la langue choisie dans l'interface (français par défaut,
anglais possible).

---

## 12. Compiler et flasher

### 12.1 Prérequis

- [ESP-IDF](https://github.com/espressif/esp-idf) **v5.3.2**, la version avec
  laquelle le projet est compilé et testé ;
- une machine de développement Linux ou macOS avec Python 3 ;
- un port USB pour la carte.

Les dépendances matérielles sont récupérées automatiquement au premier
configuration : le pilote d'écran est présent dans `main/lcd/` et le système de
fichiers `joltwallet/littlefs` est résolu par le gestionnaire de composants.

### 12.2 Compilation

```bash
. $IDF_PATH/export.sh                # active l'environnement ESP-IDF
idf.py set-target esp32s3            # une seule fois
idf.py reconfigure                   # récupère les composants gérés
idf.py build
```

Le résultat est `build/rfsentinel.bin`, accompagné de `build/bootloader.bin` et
`build/partition-table.bin`.

### 12.3 Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor    # adapter le port à votre machine
```

Pour flasher les trois images manuellement, en respectant les adresses de la table
de partitions et les paramètres de la mémoire :

```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 build/bootloader.bin \
    0x8000 build/partition-table.bin \
    0x10000 build/rfsentinel.bin
```

Les images publiées sur la page des versions de ce dépôt s'installent de la même
façon, sans avoir à compiler.

> **Attention au modèle de carte.** Deux conditions doivent être réunies : 16 Mo de
> mémoire flash et l'écran ST7735. Une carte de 8 Mo — même boîtier, même apparence —
> refusera le démarrage, et une carte vendue sans écran n'offre rien à piloter au
> firmware. La cible est la LilyGO T-Dongle-S3 **avec écran, en version 16 Mo** ; il
> existe sur le marché des variantes plus petites et des versions sans écran.

### 12.4 Flasher depuis le navigateur

Le répertoire `webtool/` contient une page autonome qui écrit le firmware par
l'API Web Serial, sans rien installer côté utilisateur. Elle n'existe que pour
Chrome et Edge, seuls navigateurs qui exposent le port série, et le navigateur
n'autorise l'accès que depuis HTTPS, `localhost` ou `file://`. Le déploiement
conseillé est la copie servie sur <https://upd.rnd0.fr/rfsentinel/>, où la page et
les images viennent du même serveur ; les montages locaux décrits plus bas restent
des solutions de repli.

Le moteur choisi n'est pas `esp-web-tools`, la solution la plus répandue : ses
éléments écrivent avec `flashMode`, `flashFreq` et `flashSize` à `keep`,
c'est-à-dire les réglages déjà présents dans la carte. Or le risque principal du
projet est une carte de 8 Mo recevant une table de partitions de 16 Mo. Le
flasher RFSentinel est donc une page propre, posée sur `esptool-js` 0.7.1, qui
impose `dio`, `80m` et `16MB`, lit la capacité réelle avant d'écrire et
refuse toute carte plus petite. Le manifeste ne fournit que des URL et des
empreintes : jamais une adresse ni un réglage mémoire, et les adresses `0x0`,
`0x8000` et `0x10000` viennent du code de la page.

La page est bilingue français / anglais. Le sélecteur de l'en-tête traduit tous les
libellés, y compris les messages lus sur la carte ; le choix est enregistré dans le
navigateur et la première visite suit sa langue. Traduire la page ne change jamais le
plan d'écriture : les adresses, les réglages mémoire et le contrôle de capacité
restent ceux décrits ci-dessus.

La page doit marcher hors ligne, donc sans import de module ES, ce que Chrome
interdit en `file://`. Le bundle d'esptool-js, livré en modules ES, est converti
en script classique par `tools/make_webtool_vendor.py` : la clause `export`
finale devient une affectation à un global, et le résultat est vérifié par
`node --check`.

Le mode en ligne lit `webtool/firmware/manifest.json` et les trois images qui le
suivent, d'où la nécessité de committer ce dossier.
`tools/package_webtool_firmware.py` le remplit depuis `build/` et refuse de
copier quoi que ce soit si le build n'annonce pas `esp32s3` en `dio/80m/16MB`.
Les pièces jointes d'une publication GitHub ne renvoient pas d'autorisation CORS :
le firmware est donc chargé depuis les fichiers bruts du dépôt ou GitHub Pages,
qui en renvoient.

Après l'écriture, esptool-js relit l'empreinte MD5 de la flash et la compare à
celle de l'image envoyée ; c'est la page qui fournit cette fonction de hachage,
`SubtleCrypto` étant indisponible en contexte non sécurisé pour le SHA-256 des
images téléchargées. La partition `storage` n'étant pas réécrite, le journal des
observations survit à une écriture normale ; l'effacement total reste un choix
explicite de l'utilisateur.

```bash
idf.py build
python3 tools/package_webtool_firmware.py
python3 -m http.server 8000      # puis http://localhost:8000/webtool/
node tests/test_webtool_ui.js    # md5, manifeste, contrôle 8/16 Mo, progression
```

### 12.5 Configuration (`sdkconfig.defaults`)

| Option | Valeur | Pourquoi |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | `y` | 16 Mo de mémoire flash |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | table décrite au §3.1 |
| PSRAM | désactivée | la carte n'en expose pas d'exploitable ici |
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | `y` | scans BLE actifs |
| `CONFIG_BT_NIMBLE_ROLE_OBSERVER` | `y` | réception des publicités passives |
| `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` | `n` | l'appareil ne se fait pas connecter |
| `CONFIG_BT_NIMBLE_ROLE_BROADCASTER` | `n` | il n'émet pas de publicités |
| `CONFIG_LWIP_MAX_SOCKETS` | `12` | interface, MQTT, NTP et WiGLE en parallèle |
| `CONFIG_FREERTOS_HZ` | `1000` | temporisations de scan au millième |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | `6144` | marge pour la boucle principale |
| bundle de certificats mbedTLS | complet | HTTPS vers l'API WiGLE |
| niveau du journal | `INFO` | lisible sur le port série sans noyer le suivi |

### 12.6 Images livrées en v1.0.0

| Fichier | Taille | SHA-256 |
|---|---|---|
| `rfsentinel.bin` | 1 482 464 o | `65ec22d5a8daac6222d0a90345e6e737490440ceeb8c32f8a4fcf8571cc17fa2` |
| `bootloader.bin` | 21 648 o | `f7e8d5acf0157d246f2594c415728fe98a15e294ab8a0c1cbfe4a61841fe5297` |
| `partition-table.bin` | 3 072 o | `4c41038930270e3e0073c60762b35cd693ad237238c335617ba9a03a0cfdf913` |

Paramètres de flash correspondants : `--flash_mode dio --flash_freq 80m
--flash_size 16MB`, aux adresses `0x0`, `0x8000` et `0x10000`.

Ces trois images sortent d'une compilation propre sous ESP-IDF v5.3.2, sans erreur.
Une compilation de référence produit un avertissement sans effet fonctionnel : la
fonction `draw_frame` de `display.c` est conservée mais non appelée.

Les mêmes fichiers, aux mêmes empreintes, sont déposés dans `webtool/firmware/` par
`tools/package_webtool_firmware.py` : le flasher du dépôt sert ce dossier, en plus
des images chargées depuis la page des versions. Les deux sources doivent rester
cohérentes, sous peine de proposer selon le bouton choisi deux firmwares différents
portant le même numéro de version. Après chaque compilation destinée à être publiée,
relancer l'outil puis vérifier que `webtool/firmware/SHA256SUMS` correspond toujours
au contenu de `build/`.

---

## 13. État du projet

La version 1 est terminée : les fonctions annoncées sont écrites, compilées et
utilisées. Pour rester honnête sur ce qui est garanti, l'état est séparé en trois
niveaux.

**Écrit et testé sur machine de développement** — hors cible, dans `tests/` :
conversion du format d'export, tri et filtres de la liste des observations,
conversion UTC vers l'heure locale, bornes du formatage, traitement des entrées
inattendues.

**Observé sur la carte** — comportement vérifié à l'écran ou dans l'interface après
flash : allumage et lisibilité de l'écran, rotation et police corrigées, connexion
et changement de mot de passe, lancements de scans, remplissage du journal,
interface complète, bascule en point d'accès de secours, affichage de l'heure.

**Écrit, compilé, mais à revoir sur la carte après la dernière image** : tuiles
OpenStreetMap sur la page de recherche, masquage et révélation des six champs
secrets de la page de configuration, heure locale dans les listes et les filtres,
téléversement réel vers WiGLE, purge réelle du journal, téléchargement réel du
CSV, calibration, recherche BLE et alertes de présence. Rien n'indique un problème
sur ces points ; ils n'ont simplement pas été revérifiés un par un sur l'équipement
depuis la dernière compilation.

**Non réalisé** : aucune mesure de portée radio, d'autonomie ou de comportement en
mobilité n'a été menée. Ce sont des validations de terrain, à la main.

---

## 14. Évolutions possibles

Des pistes, dans l'ordre où elles apporteraient le plus à l'usage :

- **Plusieurs appareils, un seul tableau.** Chaque appareil porte son identifiant ;
  il manque une page qui fusionne les journaux de plusieurs nœuds, et une
  synchronisation entre nœuds pour écarter les doublons.
- **Itinéraire.** Les observations portent une position quand un GPS est branché ;
  enregistrer la trace suivie permettrait de colorer la cartographie par instant
  plutôt que par seule intensité de signal.
- **Zones et seuils d'alerte.** Les alertes MQTT et la surveillance d'adresses
  choisies existent, mais le seuil d'alerte générale reste unique. Des zones
  dessinées sur la carte, chacune avec son seuil, affineraient la surveillance.
- **Historique des réseaux nouveaux.** L'appareil sait repérer une adresse jamais
  vue ; il ne tient pas encore de journal consultable avec date de première
  rencontre et première position.
- **Reprise d'upload.** Un téléversement WiGLE qui échoue laisse le journal intact,
  ce qui est le comportement sûr ; une file qui retente seule éviterait d'y
  revenir à la main.
- **Compteur à l'écran.** L'écran de statut reste sobre ; afficher le nombre
  d'observations écrites depuis le démarrage donnerait une preuve immédiate de
  fonctionnement sans ouvrir l'interface.
- **Lots plus grands vers WiGLE.** Le tampon de 6 Ko borne la taille d'un envoi ;
  un découpage en tranches successives permettrait de remonter un journal complet
  en une seule opération.
- **Chiffrement de l'interface.** Un HTTPS embarqué supprimerait la circulation en
  clair des identifiants, au prix d'une place notable en mémoire.

---

## 15. Risques et limites connues

| Risque | Effet | Garde-fou en place |
|---|---|---|
| Carte de 8 Mo au lieu de 16 Mo | la partition de données dépasse la mémoire, la carte ne démarre pas | avertissement au §12.3, taille explicite dans la table de partitions |
| Mémoire interne seule, sans PSRAM | impossible de garder un gros lot en RAM | tout transite par le journal, tampons bornés, libérations sur toutes les sorties |
| Journal saturé | les écritures s'arrêtent | seuil d'alerte à 90% d'occupation, purge commandée depuis l'interface |
| Horodatage faux | données inexploitables en cartographie | qualité de temps enregistrée par observation et reprise dans l'export |
| Perte du mot de passe administrateur | accès perdu | réinitialisation complète par effacement de la mémoire non volatile |
| Écoute du réseau local | identifiants et données circulent en clair | usage prévu sur réseau maîtrisé ; cookie de session `HttpOnly` et `SameSite=Strict`, verrouillage après trois échecs |
| Captures Wi-Fi et BLE | l'appareil enregistre des identifiants de réseaux environnants | publication et téléversement laissés à la décision de l'utilisateur, jamais automatiques |
| Pilote d'écran sans en-tête de licence | origine du fichier peu lisible pour un repreneur | mentionnée dans `LICENSE` |

Ce qui relève de la loi, et non du code : capter le trafic des réseaux voisins et
le publier sur une plateforme dépend de la réglementation du pays d'utilisation. Le
téléversement vers WiGLE est un clic de l'utilisateur, jamais un automatisme, et
les identifiants de la plateforme ne sont jamais renvoyés par l'appareil.

---

## 16. Utiliser RFSentinel

### 16.1 Mise en route

1. Flasher les trois images (§12.3), ou les images publiées en version.
2. Brancher la carte sur un port USB ou un chargeur 5 V. L'écran affiche le logo,
   puis le numéro de version et l'adresse obtenue.
3. Rejoindre le réseau proposé. Au premier lancement, aucun Wi-Fi n'est
   enregistré : l'appareil ouvre lui-même le point d'accès `RFSentinel-Setup`, mot
   de passe `012345678`.
4. Ouvrir `http://192.168.4.1` depuis ce même réseau.

### 16.2 Première connexion

Le compte de départ est `admin` / `admin`. La connexion ouvre obligatoirement un
écran de changement de mot de passe : sans nouveau mot de passe, l'interface reste
inaccessible. C'est le seul moyen de protéger l'appareil, puisqu'il écoute sur le
réseau local.

Une fois connecté, la page de configuration permet de renseigner le Wi-Fi cible, le
fuseau, l'identifiant de l'appareil, et éventuellement MQTT et WiGLE. Après
enregistrement du Wi-Fi, l'appareil redémarre sur ce réseau et l'adresse affichée à
l'écran devient celle qu'il faut utiliser.

### 16.3 Usage courant

Les scans démarrent seuls au lancement, si l'option est laissée active. La page de
suivi montre l'état du Wi-Fi et du Bluetooth, le remplissage du journal et les
dernières observations, triables et filtrables. La page de cartographie place les
observations et les réseaux connus sur un fond OpenStreetMap. La page
d'export prépare le fichier au format WiGLE, et la page d'upload l'envoie à la
plateforme quand on le demande.

On peut aussi dresser la liste des adresses MAC à surveiller : chacune
déclenche une alerte MQTT à la détection, quelle que soit la force du signal,
puis une nouvelle alerte après absence.

Les seuils de signaux faibles et le seuil d'alerte MQTT se règlent dans
la configuration ; ils agissent dès le scan suivant.

### 16.4 Ce que RFSentinel ne fait pas

- Il ne se connecte à aucun des réseaux qu'il détecte et ne teste aucun mot de
  passe. Il écoute les trames que les bornes diffusent publiquement.
- Il ne localise pas par lui-même : sans GPS branché, les coordonnées restent à
  zéro et l'export l'indique.
- Il ne téléverse rien vers WiGLE sans qu'on le lui demande. Les seules
  publications automatiques sont les alertes MQTT, et seulement si MQTT est
  activé.
- Il ne remplace pas un audit réseau : il produit une base de données de
  couverture, pas un diagnostic de conformité.

---

## 17. Reprendre le projet

### 17.1 Ordre de lecture

1. `README.md` : ce que fait l'appareil, sur quelle carte, comment compiler.
2. Ce fichier : le pourquoi des choix, le détail des mécanismes, les valeurs.
3. Le code, dans l'ordre du §3.1 : `config` et `store` d'abord, car presque tout
   le reste lit ou écrit chez eux.

### 17.2 Tests sur machine

Le dossier `tests/` contient trente-et-un programmes autonomes, exécutés sur la machine
de développement sans ESP-IDF ni matériel : vingt en Python, neuf en JavaScript
passé par Node, deux en C. Ils portent sur la logique qui n'a pas besoin de la carte :
format de l'export CSV, tri et filtres, conversions de temps et bornes de
formatage, seuils d'enregistrement et d'alerte MQTT, déduplication, équipements
surveillés sous toutes leurs coutures — API, logique, charge utile MQTT, interface —,
recomptage, accès concurrents au journal, itération sur un LittleFS fragmenté,
recherches WiGLE en lecture, en résumé et en TLS, lecture de l'export, classification
et filtrage des adresses BLE, et quatre programmes qui compilent un morceau du
firmware sur l'hôte :
`test_display_lifecycle_host.py` rejoue la bascule d'extinction avec un transfert
DMA simulé et vérifie que le framebuffer n'est rendu qu'une fois ce transfert terminé
(§11.5) ; `test_wigle_memory_host.py` exécute le vrai code de recherche WiGLE avec
un HTTPS simulé et des allocations en échec, pour vérifier que tous les blocs sont
libérés sur les sorties anticipées ; `test_ble_config_host.py` exécute la vraie fonction
de réglage du filtre BLE avec une NVS simulée, en succès comme en échec d'écriture ;
`test_fresh_storage_host.py` exécute la décision prise par `store_init()` après un
échec de montage, sur une partition vierge, une qui contient déjà un journal, une
corrompue, une dont la lecture échoue, une chiffrée et une absente.

```bash
for t in tests/test_*.py; do python3 "$t"; done
for t in tests/test_*.js; do node "$t"; done

cc -Imain tests/test_ble_address.c -o /tmp/bleaddr && /tmp/bleaddr

LFS=managed_components/joltwallet__littlefs/src/littlefs
cc -I"$LFS" tests/test_littlefs_iter_host.c "$LFS/lfs.c" "$LFS/lfs_util.c" \
   -o /tmp/lfs_iter && /tmp/lfs_iter
```

Les programmes Python et JavaScript se lancent tels quels, sans dépendance
extérieure. `test_ble_address.c` se compile avec un simple `-Imain`, la classification
des adresses n'ayant aucune dépendance ESP. L'autre se compile contre la
bibliothèque LittleFS récupérée par ESP-IDF, d'où le `-I` sur le dossier de
composants ; la compilation doit intervenir après un `idf.py reconfigure`, seul
capable de créer ce dossier. Il n'y a pas d'orchestrateur unique dans le dépôt :
chaque programme affiche ce qu'il vérifie et sort avec un code non nul en cas
d'échec.

Ces tests ne prouvent rien sur la radio, l'écran ou le système de fichiers : ils
couvrent le traitement des données, pas la cible.

### 17.3 Conventions du code

- Une fonctionnalité n'est tenue pour opérationnelle que si elle a été observée sur
  la carte. Une compilation sans erreur ne suffit pas, et le §13 distingue
  explicitement les deux.
- Toute valeur visible d'un utilisateur passe par une constante nommée dans un
  en-tête, jamais par un nombre écrit au milieu d'un traitement.
- Les tampons ont une taille maximale connue à l'avance, et chaque sortie anticipée
  libère ce qui a été alloué. C'est la conséquence directe de l'absence de PSRAM.
- Les champs sensibles ne sortent jamais de l'appareil vers l'interface : ils ne
  sont pas renvoyés par l'API, et l'interface ne les réaffiche pas.
- Le français est la langue de référence ; les chaînes affichées existent dans les
  deux langues et se choisissent par `ui_lang`.

### 17.4 Ce qui n'a pas sa place dans le dépôt

`.gitignore` tient le dépôt léger et ne laisse entrer que ce qui sert à compiler,
comprendre ou utiliser l'appareil. Ce qui reste dehors, et pourquoi :

| Exclu | Motif |
|---|---|
| `build/`, `managed_components/`, `sdkconfig` | produits par `idf.py` sur la machine de qui compile ; `sdkconfig` porte les réglages locaux |
| `.build*.log`, `*.log`, `backups/` | journaux de compilation et sauvegardes du journal radio réel, donc des relevés d'environnement |
| `*.bin`, `*.elf`, `*.map` | images de flash ; la version publiée est celle de la page des versions |
| `./*.h`, `/lcd/`, SVG à la racine | duplicatas de travail ; la source de vérité est dans `main/` et `logo/` |
| `notes/`, `01-*.md`, `02-*.md`, `03-*.md`, `PROGRESS.md` | journal de conception interne, avec adresses et relevés réels |
| `._*`, `.DS_Store` | métadonnées macOS |
| `.env`, `*.pem`, `*.key`, `credentials*`, `secrets*` | rien de confidentiel n'a sa place dans l'histoire du dépôt |

Une exception assume la règle : `!webtool/firmware/*.bin`. Le flasher du dépôt a
besoin de ses trois images pour fonctionner sans téléchargement préalable — le mode
« Depuis le dépôt » ; elles sont donc suivies malgré `*.bin`. Le point à surveiller
est là : la page des versions et ce dossier doivent porter les mêmes empreintes (§12.6).

`screen/` ne contient que les cinq captures de la page d'accueil, ramenées au
double de la largeur où le README les affiche — 1012 px de large maximum pour la
page des observations, 940 px pour celles d'un tableau à deux colonnes — afin de
rester net sur un écran Retina sans traîner des pixels que personne ne verra.
Le fichier de connexion fait exception : 23 Ko en 1400 px, et tout
rééchantillonnage le ferait grimper à 177 Ko parce que l'anti-crénelage détruit
la compression des aplats. Avant publication, repasser `exiftool` sur ces
fichiers : aucun ne doit porter d'horodatage, de marque, de modèle, de
coordonnées ni de XMP.

Avant toute publication, vérifier ce qui partirait réellement, puis relire cette
liste à la recherche d'une identité, d'une adresse réseau ou d'un secret oublié :

```bash
git ls-files --others --exclude-standard | while IFS= read -r f; do
  grep -InH -E 'franck|rondot|contact@|/home/|/Volumes/|/Users/|[0-9]{1,3}([.][0-9]{1,3}){3}' "$f"
done
```

Les seules adresses IP que cette recherche doit laisser trouver sont celle de l'AP
de secours, `192.168.4.1`, constante de `main/net.h`, et le `192.168.1.1` donné en
exemple dans le champ broker de la page de configuration. Les identifiants de
première mise en route — `admin`, `RFSentinel-Setup`, `012345678` — sont attendus
aussi : ce sont des valeurs par défaut documentées, pas des secrets.

---

## 18. Constantes de référence

Toutes les valeurs ci-dessous sont lues dans le code, avec le nom qui les porte.

### 18.1 Identité et accès

| Constante | Valeur | Emplacement |
|---|---|---|
| `RFS_VERSION` | `1.0.0` | `main/version.h` |
| `CFG_AP_SSID` | `RFSentinel-Setup` | `main/config.h` |
| `CFG_AP_PASS` | `012345678` | `main/config.h` |
| `NET_AP_DEFAULT_IP` | `192.168.4.1` | `main/net.h` |
| `CFG_DEFAULT_USER` / `CFG_DEFAULT_PASS` | `admin` / `admin` | `main/config.c` |
| `NVS_NS` | `rfsentinel` | `main/config.c`, `net.c`, `store.c` |
| `SESSION_TTL_US` | 1 h | `main/auth.c` |
| Cookie de session | `sid; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600` | `main/auth.c` |
| Verrouillage de connexion | 30 s après 3 échecs | `main/auth.c` |
| `FALLBACK_EPOCH` | `1735689600` (2025-01-01 UTC) | `main/net.c` |
| `TZ_DEFAULT` | `CET-1CEST,M3.5.0,M10.5.0/3` | `main/net.c` |
| Serveur NTP par défaut | `pool.ntp.org` | valeur de repli dans `main/config.c` |
| `NTP_RESYNC_INTERVAL_S` | 21 600 s (6 h) | `main/net.c` |
| `TIME_REF_SAVE_INTERVAL_S` | 3 600 s | `main/net.c` |
| `STA_CONNECT_TIMEOUT_MS` | 8 000 ms | `main/net.c` |
| `STA_MAX_RETRY` | 4 | `main/net.c` |

### 18.2 Scans

| Constante | Valeur | Emplacement |
|---|---|---|
| `SCAN_WIFI_INTERVAL_S` | 4 s | `main/scanner.c` |
| `SCAN_WIFI_MAX_APC` | 40 réseaux par tour | `main/scanner.c` |
| `SCAN_BLE_INTERVAL_S` | 20 s | `main/scanner.c` |
| `SCAN_BLE_DURATION_MS` | 10 000 ms | `main/scanner.c` |
| `SCAN_BLE_MAX` | 12 appareils par tour | `main/scanner.c` |
| `CALIBRATION_TIMEOUT_MS` | 15 min | `main/scanner.c` |
| `SCANNER_FIND_MAX` | 48 | `main/scanner.h` |
| Mode de scan | passif, `filter_duplicates=1` | `main/scanner.c` |
| Filtre des adresses BLE | 0 toutes, 1 stables, 2 publiques | `main/ble_address.h` |
| Diagnostic BLE | 5 compteurs, saturation à 4 294 967 295 | `main/scanner.c` |

### 18.3 Journal et données

| Constante | Valeur | Emplacement |
|---|---|---|
| `RFS_MOUNT` | `/rfs` | `main/store.c` |
| `RFS_OBS_DIR` | `/rfs/observations` | `main/store.c` |
| `RFS_OBS_PREFIX` | `obs_` | `main/store.c` |
| `RFS_OBS_MAX_FILES` | 32 fichiers | `main/store.c` |
| `RFS_SEGMENT_MAX_BYTES` | 32 768 o | `main/store.c` |
| `RFS_LINE_MAX` | 256 o | `main/store.c` |
| `RFS_LIST_PAGE` | 14 lignes par page | `main/store.c` |
| `RFS_SORT_MAX` | `RFS_LIST_PAGE + 1` | `main/store.c` |
| `RFS_HIST_MAX` | 10 entrées | `main/store.c` |
| `RFS_USAGE_LIMIT_PCT` | 90 % | `main/store.c` |
| `SEEN_CAPACITY` | 128 adresses connues | `main/store.c` |
| `BL_CACHE_MAX` | 64 | `main/store.c` |
| `BL_INDEX_MAX` | `BL_CACHE_MAX * 18` | `main/store.c` |
| `OBS_SSID_MAX` / `OBS_AUTH_MAX` | 33 / 16 | `main/store.h` |

L'en-tête d'export est `WigleWifi-1.6,apssids=true,latlon=true,
lockedToNetwork=false,cellBands=auto`.

### 18.4 MQTT

| Constante | Valeur | Emplacement |
|---|---|---|
| `MQTT_SEEN_MAX` | 16 | `main/mqtt.c` |
| `MQTT_URL_MAX` | 96 | `main/mqtt.c` |
| `OBS_MAC_STR` / `OBS_TS_STR` | 18 / 20 | `main/mqtt.c` |
| Sujet par défaut | `/rfsentinel/alert` | `main/config.c` |
| Identifiant client par défaut | `rfsentinel` | `main/config.c` |
| Port par défaut | 1883, sans TLS | `main/config.c` |
| Qualité de service | 1, sans rétention | `main/mqtt.c` |
| Taille de message | 384 o au plus | `main/mqtt.c` |
| `PRESENCE_MAX` / `PRESENCE_LABEL_MAX` | 32 / 32 | `main/presence.h` |
| `PRESENCE_RETRY_US` | 5 s | `main/presence.c` |
| Espace NVS des surveillances | `rfs_presence` | `main/presence.c` |
| Sujet de présence par défaut | `/rfsentinel/presence` | `main/presence.c` |
| Absence avant alerte de retour | 120 s, 30 s minimum | `main/presence.c` |
| Message de présence | 512 o au plus | `main/mqtt.c` |

### 18.5 Écran

| Constante | Valeur | Emplacement |
|---|---|---|
| `RFS_SCREEN_W` / `RFS_SCREEN_H` | 160 / 80 | `main/display.c` |
| `RFS_LINE_H` | 16 px | `main/display.c` |
| `RFS_MARGIN` | 4 px | `main/display.c` |
| `RFS_LOGO_H` | 64 px | `main/display.c` |
| `OLED_LOGO_W` / `OLED_LOGO_H` | 64 / 64 | `main/oled_logo.h` |
| Signature de démarrage | `By M3r0W1g` | `draw_credit` dans `main/display.c` |
| `RFS_BOOT_HOLD_MS` | 60 000 ms, soit 1 min | `main/display.c` |
| Framebuffer | 25 600 o en `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL`, libere a la veille | `main/display.c` |
| Horloge SPI | 20 MHz | `main/display.c` |

| Broche | GPIO | Rôle |
|---|---|---|
| `RFS_PIN_LCD_CLK` | 5 | horloge SPI |
| `RFS_PIN_LCD_MOSI` | 3 | données |
| `RFS_PIN_LCD_DC` | 2 | registre ou image |
| `RFS_PIN_LCD_RST` | 1 | réinitialisation |
| `RFS_PIN_LCD_CS` | 4 | sélection d'esclave |
| `RFS_PIN_LCD_BL` | 38 | rétroéclairage, actif à l'état bas |

### 18.6 Tampons de l'interface

| Constante | Valeur | Emplacement |
|---|---|---|
| `RFS_UI_BUF` | 4 096 o | `main/web.c` |
| `RFS_OBS_BUF` | 8 192 o (repli à 4 096 o) | `main/web.c` |
| `RFS_HIST_BUF` | 8 192 o | `main/web.c` |
| `RFS_POST_MAX` | 4 096 o | `main/web.c` |
| `RFS_WIGLE_BUF` | 8 192 o | `main/web.c` |
| `RFS_EXPORT_BUF` | 4 096 o | `main/web.c` |
| `WIGLE_BODY_MAX` | 6 144 o | `main/wigle.c` |
| `WIGLE_MAX_ROWS` | 10 lignes | `main/wigle.c` |
| Gestionnaires HTTP | 28 déclarés pour 27 routes | `main/web.c` |

### 18.7 Valeurs de première configuration

Ces valeurs s'appliquent tant que l'utilisateur n'a rien changé.

| Réglage | Valeur initiale |
|---|---|
| Seuil de signal faible | -95 dBm |
| Seuil d'alerte MQTT | -60 dBm |
| Délai de redécouverte | 60 min |
| Scan Wi-Fi, scan BLE, démarrage automatique | actives |
| Filtre des adresses BLE | stables : publiques et aléatoires statiques |
| Langue | français |
| MQTT | désactivé, port 1883, sans TLS |
| Identifiant de l'appareil | `rfsentinel-01` |
| Position | latitude 0, longitude 0 |
| Serveur NTP | `pool.ntp.org` |
| Fuseau | `CET-1CEST,M3.5.0,M10.5.0/3` |

---

## 19. Versions

| Version | Date | Contenu |
|---|---|---|
| 1.0.0 | novembre 2026 | première version complète, images reprises avec le correctif de carte neuve |

La version 1.0.0 réunit les scans Wi-Fi et BLE, le journal sur carte, l'interface
web en français et en anglais, la cartographie, l'export au format WiGLE, le
téléversement vers WiGLE, les alertes MQTT, l'écran et le point d'accès de secours.
Les images publiées plus tard sous ce même numéro corrigent le démarrage sur une carte
jamais flashée : la partition `storage` y restait définitivement non montée, sans
moyen d'enregistrer les observations. Le numéro dans `main/version.h` n'a pas bougé,
puisque les fonctions annoncées sont les mêmes.

Le numéro est défini une seule fois, dans `main/version.h`, et repris par l'API,
l'écran et l'interface. Une version publiée n'est pas modifiée après coup : les
corrections vont dans la version suivante.

---

## 20. Où regarder dans le code

| Fichier | Rôle |
|---|---|
| `main/main.c` | démarrage, ordre d'initialisation, boucle principale |
| `main/version.h` | numéro de version |
| `main/config.c` | réglages persistes en mémoire non volatile |
| `main/store.c` | journal sur carte, liste, tri, filtres, purge, liste noire |
| `main/scanner.c` | scans Wi-Fi et BLE, calibration, compteurs d'adresses BLE |
| `main/ble_address.h` | classification et filtrage des adresses BLE |
| `main/net.c` | connexion, point d'accès de secours, NTP, heure locale |
| `main/auth.c` | sessions, verrouillage après échecs |
| `main/web.c` | routes HTTP, pages, API, export |
| `main/ui/index.html` | interface complète, d'une seule pièce |
| `main/mqtt.c` | alertes |
| `main/presence.c` | adresses surveillées, alertes de présence |
| `main/wigle.c` | téléversement et recherche |
| `main/display.c` | framebuffer, rotation, accueil, statut, veille |
| `main/oled_logo.h` | logo 64 x 64 |
| `main/lcd/esp_lcd_st7735.c` | pilote du contrôleur d'écran |
| `partitions.csv` | répartition de la mémoire flash |
| `sdkconfig.defaults` | options de compilation |
| `tests/` | tests hors cible |

---

## 21. Licence

RFSentinel est publié sous **Creative Commons Attribution - Usage non commercial
- Partage dans les mêmes conditions 4.0 International** (CC BY-NC-SA 4.0). Le texte
complet est dans `LICENSE`.

Concrètement :

- vous pouvez copier, modifier, adapter, compiler pour votre carte, diffuser ;
- à condition de citer l'auteur ;
- à condition de ne pas en faire un usage commercial ;
- et à condition que toute version modifiée ou dérivée reste sous la même licence,
  donc libre et sans usage commercial à son tour. Un fork reste ouvert.

Un usage commercial reste possible, mais seulement avec un accord écrit de
l'auteur.

Deux précisions honnêtes pour un repreneur :

- CC BY-NC-SA n'est pas une licence approuvée par l'Open Source Initiative, parce
  que la clause non commerciale est jugée discriminante par cette définition. Le
  projet assume ce choix : l'objectif est la diffusion libre avec interdiction
  d'exploitation commerciale, pas la conformité à cette définition.
- le composant `joltwallet/littlefs`, récupéré par le gestionnaire de composants,
  reste sous Apache-2.0 et suit ses propres termes. Le pilote
  `main/lcd/esp_lcd_st7735.c` ne porte pas d'en-tête de licence ; cette origine
  incertaine est signalée dans `LICENSE`.
