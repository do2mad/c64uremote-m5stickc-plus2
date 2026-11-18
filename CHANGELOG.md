# Änderungen / Changelog

C64uRemote für den **M5StickC Plus2**. Neueste Version zuerst.

## v1.6.0 – 2026-11-19

### Deutsch

**Neu: c64u per Wake on LAN wecken.** Ab der c64u-Firmware 1.2 lässt sich der
c64u über WLAN aufwecken (im c64u unter *Power Settings* die Einstellung
*Wake on LAN* einschalten). Geweckt wird an die WLAN-MAC-Adresse, nicht an die
IP-Adresse.

- Die MAC holt sich der Stick selbst, sobald er den c64u einmal erreicht hat
  (`/v1/info`, Feld `wifi_mac`), und merkt sie sich dauerhaft.
- Fehlt der c64u ein paar Sekunden, öffnet sich die Rückfrage „c64u wecken?“ mit
  NEIN/JA (B oder Joystick wählt, Power-Taste oder Joystick-Knopf bestätigt, A = NEIN) – einmal pro Ausfall. Nach dem Wecken schaut der Stick in kurzen Abständen nach, bis der c64u da ist.
- Solange der c64u fehlt, öffnen der Menüpunkt PowerOff (dann „Wecken“) und Joystick nach unten dieselbe Rückfrage, statt auszuschalten. Ohne Bestätigung wird nie geweckt.
- Wecken auch im Direktmodus: Der Weckruf geht dann an das eigene Funknetz des Stick.
  Von selbst gefragt wird dort nur, wenn ein Gerät angemeldet ist (der schlafende c64u)
  und trotzdem 15 s lang kein c64u antwortet.
- Die Rückfrage zeigt die Auswahl deutlich: Die gewählte Taste ist grün (JA)
  bzw. rot (NEIN) gefüllt, darunter steht, was die Bestätigung auslöst.
- POWER weckt auch direkt nach dem Einschalten, solange der c64u noch nicht
  geantwortet hat – und im Direktmodus auch dann, wenn kein c64u angemeldet ist.
- JA, bevor das WLAN steht: Es erscheint „WECKRUF FOLGT, SOBALD WLAN DA“, der
  Weckruf geht dann von selbst hinaus.
- Der Weckruf wird 30 s lang alle 0,7 s wiederholt, bis der c64u antwortet – ein
  schlafender c64u hört nur in Abständen ins WLAN. Einmal JA genügt.
- Ist der c64u nicht erreichbar und Wecken nicht möglich, meldet POWER „c64u
  NICHT ERREICHBAR“, statt nach dem Ausschalten zu fragen.

### English

**New: wake the c64u with Wake on LAN.** From c64u firmware 1.2 on the c64u can
be woken over WiFi (enable *Wake on LAN* under *Power Settings* on the c64u).
The wake-up goes to the WiFi MAC address, not to the IP address.

- The Stick fetches the MAC itself as soon as it has reached the c64u once
  (`/v1/info`, field `wifi_mac`) and keeps it.
- If the c64u is missing for a few seconds, the request "Wake c64u?" opens with
  NO/YES (B or joystick chooses, power button or joystick button confirms, A = NO) – once per outage. After waking, the Stick looks for the c64u at short intervals until it is there.
- While the c64u is missing, the PowerOff menu item (then "Wake") and joystick down open the same request instead of powering off. Nothing is woken without confirmation.
- Wake-up in direct mode too: the wake-up then goes to the Stick's own WiFi.
  The request only opens by itself when a device is logged in (the sleeping c64u)
  and still no c64u answers for 15 s.
- The request shows the selection clearly: the selected button is filled green
  (YES) or red (NO), and below it the box says what confirming will do.
- POWER also wakes right after switching on, as long as the c64u has not
  answered yet – and in direct mode also when no c64u is logged in.
- YES before the WiFi is up: "WAKE-UP FOLLOWS WHEN WIFI IS UP" appears, and the
  call goes out by itself.
- The wake-up call is repeated every 0.7 s for 30 s until the c64u answers – a
  sleeping c64u only listens to the WiFi at intervals. One YES is enough.
- If the c64u cannot be reached and waking is not possible, POWER reports "c64u
  NOT REACHABLE" instead of asking to power off.

## v1.5.0 – 2026-10-18

### Deutsch

**Neu: mehrere Geräte im Direktmodus an einem Tisch.** Bisher hießen alle
Direktnetze `C64uRemote-Direct` – zwei Fernbedienungen nebeneinander kamen sich
in die Quere.

- Neuer Eintrag im WLAN-Menü: **Direkt-Nr.** Jeder Druck schaltet von *-* (ohne
  Nummer) über `C64uRemote-Direct-1` bis `-9` und zurück. Der neue Name wird kurz
  angezeigt; ein laufendes Direktnetz startet sofort damit neu.
- Jede Nummer nutzt ihren eigenen Funkkanal (1, 6, 11 im Wechsel); ohne Nummer
  bleibt es bei Kanal 6.
- Am c64u den jeweiligen Netznamen eintragen.

**Neu: Firmware im Browser installieren.** Für alle, die keine
Programmierumgebung einrichten möchten: Fertige Images lassen sich über
https://1mhz.de/M5-webflash/ in Chrome oder Edge per USB-Kabel auf den Stick spielen (esptool-js).
Die Images enthalten keine Zugangsdaten; WLAN und c64u werden danach wie gewohnt
per Karte, `wifi.txt` oder Setup-Portal eingerichtet.

**Bedienung hakt nicht mehr, wenn der c64u fehlt.** Ist der c64u aus oder
nicht im Netz, stand der Stick bisher alle 15 Sekunden für bis zu fünf Sekunden
still, während er auf eine Antwort wartete – Tasten und Menü reagierten so
lange nicht. Jetzt klopft er erst ohne Warten an (wie der M5Dial seit v1.3.1),
und erst wenn sich der c64u meldet, folgt die eigentliche Abfrage. Der CPU-Takt
wird erst nachgeladen, wenn der c64u erreichbar ist.

### English

**New: several devices in direct mode at one table.** Until now every direct
network was called `C64uRemote-Direct` – two remotes side by side got in each
other's way.

- New entry in the WiFi menu: **Direct no.** Every press steps from *-* (no
  number) through `C64uRemote-Direct-1` to `-9` and back. The new name is shown
  briefly; a running direct network restarts with it right away.
- Each number uses its own radio channel (1, 6, 11 in turn); without a number it
  stays on channel 6.
- Enter the matching network name on the c64u.

**New: install the firmware in the browser.** For everyone who does not want to
set up a development environment: ready-made images can be flashed onto the Stick
from https://1mhz.de/M5-webflash/ in Chrome or Edge over a USB cable (esptool-js). The images
contain no credentials; WiFi and the c64u are then set up as usual by card,
`wifi.txt` or setup portal.

**Operation no longer stalls when the c64u is missing.** If the c64u was off
or not on the network, the Stick used to freeze for up to five seconds every 15
seconds while waiting for an answer – buttons and menu did not respond. Now it
first knocks without waiting (like the M5Dial since v1.3.1), and only when the
c64u answers does the real query follow. The CPU speed is only fetched once the
c64u is reachable.

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
