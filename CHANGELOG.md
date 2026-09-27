# Änderungen / Changelog

C64uRemote für den **M5StickC Plus2**. Neueste Version zuerst.

## v1.4.0 – 2026-09-27

### Deutsch

**Neu: Direktmodus – ohne Router, z. B. auf Treffen.** Der Stick spannt auf Wunsch
selbst ein WLAN auf, und der c64u meldet sich direkt bei ihm an.

- Neue Einträge im WLAN-Menü: **Direktmodus** (an/aus) und **Direkt-Netz**
  (`192.168.4.x` oder `192.168.2.x`).
- Netz `C64uRemote-Direct`, Passwort `c64ultimate`. Der Stick hat die `.1`, der
  c64u bekommt per DHCP die `.64`. Eine eigene Seite zeigt, was am c64u
  einzutragen ist, und ob er schon verbunden ist.
- Meldet sich zuerst ein anderes Gerät an, sucht der Stick unter den angemeldeten
  Geräten weiter, bis der c64u antwortet.
- Ein zweiter Fernbediener kann sich als normaler Client ins Direktnetz
  einbuchen und spricht den c64u dann automatisch unter der `.64` an; seine
  Heimadresse bleibt erhalten.
- Wer ein gespeichertes Netz wählt oder eine WLAN-Karte auflegt, beendet den
  Direktmodus.
- Befehlskarten `CMD:DIRECT=192.168.4`, `CMD:DIRECT=192.168.2` und
  `CMD:DIRECT=OFF`, anzulegen unter *NFC / RFID → CMD-Karte*.

**Neu: Stick per Karte ausschalten.** Eine Befehlskarte `CMD:M5OFF` schaltet den
Stick selbst aus (am Akku ganz, an USB in den Tiefschlaf) – dieselbe Karte wie beim
M5Dial. Anzulegen unter *NFC-Cmd* bzw. *NFC / RFID → CMD-Karte*. In den ersten acht Sekunden nach dem Start
wird sie ignoriert.

**Verbindung zum c64u zuverlässiger.**

- Anfragen an den c64u (Abfragen, Befehle) laufen über einen eigenen,
  schlanken HTTP-Weg: Verbindungsaufbau ohne blockierendes Warten, die Antwort
  wird vollständig gelesen; bis zu drei Verbindungsversuche mit je 1,5 s.
- Netzname und Signalstärke werden höchstens einmal pro Sekunde beim
  WLAN-Treiber abgefragt statt tausendfach.
- Das Funkfeld des NFC-Lesers ist nur noch für die kurze Kartenprobe und
  während der Kartenbearbeitung an. Beim M5Dial hatte das dauerhaft
  eingeschaltete Feld den WLAN-Empfang gestört; hier spart es vor allem Strom.

**Versionsanzeige.** Die Statusseite zeigt jetzt die Firmware-Version im Titel
(`STATUS v1.4.0`), das Startprotokoll auf der seriellen Schnittstelle ebenfalls.

Die Nummer springt von 1.2.1 direkt auf 1.4.0: 1.3.x gab es nur für den M5Dial,
ab jetzt tragen wieder alle vier Geräte denselben Stand.

### English

**New: direct mode – no router, e.g. at meetings.** On request the Stick opens a
WiFi network of its own and the c64u connects to it directly.

- New entries in the WiFi menu: **Direct mode** (on/off) and **Direct net**
  (`192.168.4.x` or `192.168.2.x`).
- Network `C64uRemote-Direct`, password `c64ultimate`. The Stick has `.1`, the
  c64u gets `.64` via DHCP. A page of its own shows what to enter on the c64u
  and whether it is connected yet.
- If another device joins first, the Stick keeps looking among the connected
  devices until the c64u answers.
- A second remote can join the direct network as a normal client and then
  addresses the c64u at `.64` automatically; its home address is kept.
- Choosing a stored network or presenting a WiFi card ends direct mode.
- Command cards `CMD:DIRECT=192.168.4`, `CMD:DIRECT=192.168.2` and
  `CMD:DIRECT=OFF`, created under *NFC / RFID → CMD card*.

**New: switch the Stick off by card.** A command card `CMD:M5OFF` switches the
Stick itself off (completely on battery, deep sleep on USB) – the same card as on
the M5Dial. Created under *NFC-Cmd* or *NFC / RFID → CMD card*. It is ignored during the first eight
seconds after start.

**Connection to the c64u more reliable.**

- Requests to the c64u (queries, commands) go through an own, lean
  HTTP path: connecting without blocking waits, the reply is read completely;
  up to three connection attempts of 1.5 s each.
- Network name and signal strength are queried from the WiFi driver at most
  once per second instead of thousands of times.
- The NFC reader's RF field is now only on for the short card probe and while
  a card is being processed. On the M5Dial the permanently switched-on field
  disturbed WiFi reception; here it mainly saves power.

**Version display.** The status page now shows the firmware version in its title
(`STATUS v1.4.0`), and so does the boot log on the serial port.

The number jumps from 1.2.1 straight to 1.4.0: 1.3.x only existed for the M5Dial;
from now on all four devices carry the same version again.

## v1.2.1 – 2026-09-04

### Deutsch

**Stabilere Verbindung zum c64u.** Der HTTP-Server der Ultimate-Firmware weist
gelegentlich eine Verbindung ab („connection refused"), auch wenn Netz und
Adresse in Ordnung sind. Das führte bisher sofort zu *FAILED* und zu *Not
reached* in der Statuszeile.

- Ein abgewiesener Aufruf wird nach kurzer Pause **einmal automatisch
  wiederholt**. Nur bei Transportfehlern – dann ist beim c64u nichts
  angekommen, ein Befehl kann sich also nicht doppeln.
- Der zyklische Verbindungstest kostet nur noch **eine statt zwei Anfragen**,
  wenn kein Passwort hinterlegt ist. Die zweite war byte-gleich mit der ersten.
- Beim **Wiederverbinden** wird zuerst wieder das Netz versucht, mit dem es
  zuletzt geklappt hat. Sind zwei Netze gespeichert und nur eines ist
  erreichbar, wurde vorher nach jedem Aussetzer jedes zweite Mal zehn Sekunden
  am toten Netz gewartet.

Hinweis: v1.2.0 gab es nur für Core und CoreS3 (Akkuanzeige). Ab dieser Version
laufen wieder alle vier Geräte auf demselben Stand.

### English

**More robust connection to the c64u.** The HTTP server of the Ultimate firmware
occasionally refuses a connection ("connection refused") even though network and
address are fine. Until now that immediately produced *FAILED* and *Not reached*
in the status line.

- A refused call is **retried once automatically** after a short pause. Only on
  transport errors - nothing reached the c64u then, so a command cannot be
  doubled.
- The periodic connection test now costs **one request instead of two** when no
  password is stored. The second one was byte-identical to the first.
- When **reconnecting**, the network that last worked is tried first. With two
  networks stored of which only one is reachable, every other reconnect used to
  waste ten seconds on the dead one.

Note: v1.2.0 existed for the Core and CoreS3 only (battery indicator). From this
version on all four devices are back on the same footing.

## v1.1.0 – 2026-09-03

### Deutsch

**Neu: Joystickports am C64 tauschen.** Manche Spiele wollen den Joystick in
Port 1, andere in Port 2 – jetzt lässt sich das umschalten, ohne das Kabel
umzustecken.

- Der Menüpunkt **Connection Test** ist zu **Joystick Swap** geworden. Die gleiche Prüfung löst weiterhin ein Druck auf der *Status*-Seite aus. Jedes Auslösen schaltet zwischen *Normal* und *Swapped* um.
- Neue Einstellung **c64u Joystick** (hinter *Brightness*): zeigt den aktuellen Stand und
  schaltet durch **alle** Werte, die der c64u meldet – je nach Firmware auch
  *WASD Port 1* und *WASD Port 2*.
- Neue Befehlskarten: `CMD:JOY` schaltet um, `CMD:JOY=NORMAL`, `=SWAPPED`,
  `=WASD1`, `=WASD2` setzen fest. Beim Anlegen einer Karte stehen die
  Joystick-Werte zwischen den festen Befehlen und den CPU-Stufen; das Kürzel
  rechts (*JOY* / *CPU*) trennt die beiden Blöcke.
- Handbücher und README auf den neuen Stand gebracht.

**Hintergrund:** Die ReST-API der Ultimate-Firmware hat dafür keinen eigenen
`machine:`-Befehl. Die Belegung ist ein Konfigurationseintrag – im Test
*Joystick Swapper* in der Kategorie *U64 Specific Settings*. Gesetzt wird sie
über `PUT /v1/configs/<Kategorie>/<Eintrag>?value=…`, genau wie die CPU-Stufe.
Kategorie und Eintragsname sucht die Firmware zur Laufzeit (Schlüsselwort
„Joystick"), damit eine Umbenennung in einer künftigen Ultimate-Version nichts
kaputt macht.

### English

**New: swap the joystick ports on the C64.** Some games want the joystick in
port 1, others in port 2 – this can now be toggled without moving the cable.

- The menu entry **Connection Test** has become **Joystick Swap**. The same check is still triggered by a press on the *Status* page. Every trigger toggles between *Normal* and *Swapped*.
- New setting **c64u Joystick** (after *Brightness*): shows the current state and steps
  through **every** value the c64u reports – depending on the firmware also
  *WASD Port 1* and *WASD Port 2*.
- New command cards: `CMD:JOY` toggles, `CMD:JOY=NORMAL`, `=SWAPPED`, `=WASD1`,
  `=WASD2` set a fixed value. When writing a card the joystick values sit
  between the fixed commands and the CPU steps; the tag on the right
  (*JOY* / *CPU*) tells the two blocks apart.
- Manuals and README brought up to date.

**Background:** the ReST API of the Ultimate firmware has no dedicated
`machine:` command for this. The mapping is a configuration item – in testing
*Joystick Swapper* in the category *U64 Specific Settings*. It is set through
`PUT /v1/configs/<category>/<item>?value=…`, exactly like the CPU speed. The
firmware looks the category and item name up at runtime (keyword "Joystick") so
that a renaming in a future Ultimate version does not break anything.

## v1.0.0 – 2026-08-24

Erste Veröffentlichung als eigenes Repository: Firmware in deutscher und
englischer Fassung, Handbücher als PDF, MIT-Lizenz.

First release as its own repository: firmware in a German and an English
edition, manuals as PDF, MIT licence.
