# C64uRemote für M5StickC Plus2

Fernbedienung für den **Commodore 64 Ultimate (c64u)** und den **Ultimate64
Elite-II** über die ReST-API der Ultimate-Firmware (ab 3.11).

Basiert auf der Originalidee von Karl Prosser ([@klumsy](https://github.com/ReadyOS-C64/C64uRemote)),
erweitert von Martin Oswald ([@mad](https://1MHz.de)).

## Was der Stick kann

| Funktion | |
|---|---|
| Reset, Reboot, PowerOff, Ultimate-Menü | über Menü, Tasten oder NFC-Karte |
| CPU-Geschwindigkeit umschalten | Liste kommt vom c64u |
| Verbindungstest und Statusanzeige | WLAN, Erreichbarkeit, Passwort |
| **WLAN per NFC-Karte einrichten** | eine am M5Dial oder Core beschriebene Karte auflegen – kein Neuflashen |
| **Bis zu vier WLAN-Zugänge** | im internen Speicher (NVS), beim Start wird das Netz mit dem besten Empfang genommen |
| **Setup-Portal** | eigener Accesspoint mit Weboberfläche, falls keine Karte zur Hand ist |
| **Befehlskarten lesen** | `CMD:RESET`, `CMD:REBOOT`, `CMD:MENU`, `CMD:POWEROFF`, `CMD:CPU=…` |
| **Karten beschreiben** | Befehlskarten und WLAN-Karten |
| MiniJoyC (HAT-Port) | wie bisher, optional |

Programm-/Pfadkarten (Spiele) startet der Stick **nicht** – dafür bräuchte er
eine microSD mit den Dateien. Solche Karten bleiben gültig, sie gehören an
M5Dial oder M5Stack Core. Der Stick meldet das freundlich.

## Hardware

| Teil | Anschluss |
|---|---|
| M5StickC Plus2 | – |
| Unit RFID2 (WS1850S) | **Grove-Port HY2.0-4P**: SDA = G32, SCL = G33, I2C 0x28 |
| Hat MiniJoyC | HAT-Port: SDA = G0, SCL = G26, I2C 0x54 |

Beide sitzen auf **getrennten I2C-Bussen** (`Wire1` für das RFID2, `Wire` für
den MiniJoyC) und stören sich nicht. Beide werden beim Start automatisch
erkannt; fehlt eines, läuft der Rest unverändert weiter.

## Bedienung

| Taste | Startbild | Listen |
|---|---|---|
| **A** (groß, vorn) | Reset, zweimal kurz = Reboot | eine Ebene zurück |
| **B** (seitlich) | Ultimate-Menü | nächster Eintrag |
| **Power** (kurz) | Menü öffnen | auswählen / ausführen |

Mit MiniJoyC: links = zurück, rechts = auswählen, hoch/runter = blättern,
Stick drücken = Ultimate-Menü. Auf dem Startbild: hoch = Reset, runter = PowerOff.

### Menü

```
PowerOff · CPU Speed · NFC / RFID · WLAN · Connection Test · Status · Settings
```

**NFC / RFID**

| Eintrag | Bedeutung |
|---|---|
| Karte lesen | Karte auflegen, Inhalt wird ausgeführt |
| CMD-Karte | Befehl auswählen und auf eine Karte schreiben |
| WLAN auf Karte | den zuletzt benutzten WLAN-Zugang auf eine Karte schreiben |

**WLAN**

| Eintrag | Bedeutung |
|---|---|
| Von NFC-Karte | WLAN-Karte auflegen, Zugang wird übernommen und verbunden |
| Setup-Portal | Accesspoint `C64uRemote-Setup` (Passwort `c64ultimate`), Eingabe im Browser |
| Gespeichert | Liste der Netze, Auswahl verbindet |
| Auf NFC-Karte | wie oben |
| Netz löschen | einzelnes Netz verwerfen |
| Alle löschen | alle Netze verwerfen |

**Settings** enthält zusätzlich zu den Anzeige-Optionen:

| Eintrag | Bedeutung |
|---|---|
| Auto-NFC | Abstand der Hintergrundabfrage auf dem Startbild (*Off*, 1.5s, 0.7s, 0.3s) |
| NFC-Cmd PowOff | Abfragezeit einer PowerOff-Befehlskarte: 3, 5, 8, 15 s |

Steht Auto-NFC nicht auf *Off*, genügt es, eine Karte auf dem Startbild
aufzulegen – der Umweg über das Menü entfällt.

## Kartenformat

Ein gewöhnlicher **NDEF-Textrecord**, identisch zu M5Dial und M5Stack Core –
dieselbe Karte läuft an allen Geräten. Unterstützt werden NTAG213/215/216 und
MIFARE Ultralight sowie MIFARE Classic 1K/4K/Mini. Sektor-Trailer und MAD
werden nie beschrieben, eine Karte kann also nicht unbrauchbar werden.

WLAN-Karten benutzen das Schema aus dem WLAN-QR-Code:

```
WIFI:S:MeinNetz;T:WPA;P:geheim;;
```

## Bauen

```bash
cp src/build_env.h.example src/build_env.h   # ausfüllen
pio run -t upload
pio device monitor
```

In VS Code genügt es, diesen Ordner zu öffnen – die PlatformIO-IDE erkennt das
Projekt und legt `.vscode/c_cpp_properties.json` selbst an. `.vscode/extensions.json`
und `.vscode/settings.json` liegen im Projekt und schlagen beim Öffnen die
PlatformIO-Erweiterung vor.

Es gibt zwei Sprachvarianten derselben Firmware:

| Ordner | Inhalt |
|---|---|
| `src/main.cpp` | deutsche Menütexte und Kommentare |
| `src-en/main.cpp` | englische Fassung, sonst Zeile für Zeile identisch |

PlatformIO baut immer `src/`. Für die englische Fassung die Datei aus `src-en/`
nach `src/main.cpp` kopieren – genau das macht auch `build-dist.sh` beim Packen
der `-EN`-Archive.

Die Handbücher liegen als Markdown in `docs-src/` und als PDF in `doc/`;
gebaut werden sie mit `../build-docs.sh`.

`build_env.h` liefert nur noch die **Startwerte**: sobald einmal etwas über
eine WLAN-Karte oder das Setup-Portal gespeichert wurde, gilt der interne
Speicher. Die Felder dürfen deshalb auch leer bleiben.

## Board-Einstellungen – bitte nicht "aufräumen"

Für den StickC Plus2 gibt es (Stand espressif32 6.9) **keinen eigenen
Board-Eintrag** in PlatformIO. Benutzt wird deshalb der des M5StickC, aber mit
drei Korrekturen in `platformio.ini`:

| Einstellung | Warum |
|---|---|
| `board_build.extra_flags =` (leer) und `build_unflags = -DARDUINO_M5Stick_C` | Diese Kennung nagelt M5GFX auf das **alte** StickC-Panel fest. Die Erkennung des Plus2 wird dann übersprungen – **der Bildschirm bleibt dunkel.** |
| `-DM5GFX_BOARD=board_t::board_M5StickCPlus2` | setzt das Board direkt: ST7789 135 × 240, Backlight an G27, Stromhaltepin G4 |
| `-DBOARD_HAS_PSRAM` | Der ESP32-PICO-V3-02 hat 2 MB PSRAM im Gehäuse. Ohne diese Kennung wird PSRAM nicht initialisiert und `ps_malloc()` liefert nur `NULL`. |

Zusätzlich setzt `setup()` `cfg.fallback_board` auf den Plus2 – falls die
automatische Erkennung doch einmal danebengreift.

## Speicher

Die beiden Logo-Caches (je 240 × 135 × 2 = 64 kB) liegen im PSRAM; ist keins
aktiv, weicht der Code auf den internen Speicher aus. Reicht auch der nicht,
läuft das Gerät ohne Logo-Effekte weiter und meldet *WENIG SPEICHER* –
es bleibt nicht mehr wortlos hängen.

Solange das Setup-Portal läuft, werden beide Caches freigegeben und danach
wieder angelegt; Accesspoint und Webserver brauchen den Platz.

## Wenn der Bildschirm dunkel bleibt

1. `platformio.ini` prüfen – die drei Punkte aus der Tabelle oben.
2. Seriellen Monitor anschauen: `pio device monitor`. Dort steht
   `[Autodetect] M5StickCPlus2`, wenn das Panel erkannt wurde.
3. Hilft das nicht, einmal den Flash komplett löschen und neu schreiben:

   ```bash
   pio run -t erase
   pio run -t upload
   ```

   Das räumt auch den NVS-Bereich auf, in dem M5GFX ein einmal erkanntes Board
   zwischenspeichert. **Achtung:** dabei gehen gespeicherte WLAN-Zugänge und
   Einstellungen verloren.

---

## Entstehung

Portierung, Erweiterungen und Handbücher sind mit Unterstützung von
Claude (Anthropic) entstanden. Konzept, Idee, Hardware-Entscheidungen und
sämtliche Tests auf den echten Geräten: Martin Oswald (@mad).

---

## Lizenz

Dieses Projekt steht unter der **MIT-Lizenz**. Der vollständige Text liegt in
[`LICENSE`](LICENSE) im Projektstamm.

Ursprung ist das Projekt
[C64uRemote von Karl Prosser (@klumsy)](https://github.com/ReadyOS-C64/C64uRemote),
das er unter der
[MIT-Lizenz](https://github.com/ReadyOS-C64/C64uRemote/blob/main/LICENSE)
veröffentlicht hat. Diese Fassung für den M5StickC Plus2 ist eine daraus abgeleitete
Erweiterung und steht unter denselben Bedingungen:

* Copyright (c) 2026 Karl Prosser – Originalprojekt
* Copyright (c) 2026 Martin Oswald (@mad, [1MHz.de](https://1MHz.de)) – Portierung und Erweiterungen

Die MIT-Lizenz erlaubt Benutzung, Veränderung und Weitergabe – auch
kommerziell. Einzige Bedingung: **Copyright-Vermerk und Lizenztext müssen
erhalten bleiben**, also in jeder Kopie oder abgeleiteten Fassung mitgeliefert
werden. Eine Gewährleistung gibt es nicht.

### Fremde Bestandteile

Die eingebundenen Bibliotheken haben ihre eigenen Lizenzen und Copyright-Inhaber:

| Bibliothek | Lizenz |
|---|---|
| [M5Unified](https://github.com/m5stack/M5Unified) | MIT, (c) M5Stack |
| [M5GFX](https://github.com/m5stack/M5GFX) | MIT, (c) M5Stack |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT, (c) Benoit Blanchon |
| [MFRC522_I2C](https://github.com/kkloesener/MFRC522_I2C) | siehe Repository |

Sie werden von PlatformIO beim Bauen geladen und liegen diesem Archiv nicht bei.
