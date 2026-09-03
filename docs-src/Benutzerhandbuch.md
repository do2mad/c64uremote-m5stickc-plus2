% C64uRemote für M5StickC Plus2
% Benutzerhandbuch
% Version 1.0

# Willkommen

C64uRemote verwandelt einen **M5StickC Plus2** in eine handliche Fernbedienung
für den **Commodore 64 Ultimate (c64u)** und den **Ultimate64 Elite-II**. Über
das WLAN steuerst du den Rechner fern: Reset, Reboot, Ausschalten, das
Ultimate-Menü öffnen und die CPU-Geschwindigkeit umstellen.

Mit dem optionalen **Unit RFID2** am Grove-Port kommt etwas dazu, das den
kleinen Stick deutlich alltagstauglicher macht: Du richtest sein **WLAN per
NFC-Karte** ein. Eine Karte, die du am M5Dial oder am M5Stack Core beschrieben
hast, hältst du an den Stick – und er übernimmt Netz und Passwort, ohne dass er
je wieder an den Rechner muss. Dieselben **Befehlskarten** wie an den anderen
Geräten funktionieren ebenfalls, und beschreiben kann er Karten auch.

Spielekarten startet der Stick nicht – dafür fehlt ihm die Speicherkarte. Mehr
dazu im Kapitel *NFC-Karten*.

Dieses Projekt baut auf der ursprünglichen Idee von Karl Prosser (@klumsy) auf
und wurde von Martin Oswald (@mad, 1MHz.de) für den M5StickC Plus2 erweitert.

# Das brauchst du

**Zwingend erforderlich:**

- Einen M5StickC Plus2
- Einen Commodore 64 Ultimate bzw. Ultimate64 Elite-II im selben WLAN

**Optional, für NFC-Karten:**

- Ein **Unit RFID2** (WS1850S), angeschlossen an den Grove-Port (HY2.0-4P)
- NFC-Karten: empfohlen NTAG215, es gehen auch NTAG213/216 und MIFARE Classic

**Optional, zur Bedienung:**

- Einen **Hat MiniJoyC** auf dem HAT-Port

Beide Erweiterungen werden beim Einschalten automatisch erkannt. Fehlt eine,
läuft alles Übrige unverändert weiter. RFID2 und MiniJoyC liegen an getrennten
Anschlüssen und stören sich gegenseitig nicht – du kannst also beide gleichzeitig
stecken.

# Erste Einrichtung

Damit der Stick deinen C64 findet, müssen einmalig die WLAN-Zugangsdaten und die
Adresse des C64 hinterlegt werden. Dafür gibt es zwei Wege, die beide **ohne
neues Flashen** auskommen:

1. Eine **WLAN-Karte auflegen** (braucht das RFID2)
2. Das **Setup-Portal** starten und alles im Browser eintragen

Beides steht ausführlich im Kapitel *WLAN einrichten*. Die Daten landen im
internen Speicher und überstehen jeden Neustart.

Wer die Zugangsdaten lieber schon beim Programmieren mitgibt, trägt sie in
`build_env.h` ein (siehe technische Dokumentation). Sie gelten dann als
Startwerte für den allerersten Start und werden von allem überschrieben, was du
später am Gerät einstellst.

Beim ersten Start ohne hinterlegtes Netz meldet der Stick **WLAN EINRICHTEN**.
Steht das Netz, fehlt aber die Adresse des C64, meldet er **HOST FEHLT**.

## Die Status-LED des MiniJoyC

Steckt ein MiniJoyC, zeigt dessen RGB-LED den Gesamtzustand an, ohne dass du
dafür in ein Menü musst:

| LED | Bedeutung |
|---|---|
| **grün** | Alles verbunden, C64 antwortet, Passwort stimmt |
| **blau** | WLAN da, aber der C64 antwortet nicht |
| **blau blinkend** | C64 erreichbar, aber das Passwort stimmt nicht |
| **rot** | Keine WLAN-Verbindung |

Helligkeit und Ein/Aus stellst du unter *Settings* ein.

# Bedienung

Der M5StickC Plus2 hat drei Tasten: die **große Taste vorn** (A), die **kleine
Taste an der Seite** (B) und die **Power-Taste** links.

| Taste | Auf dem Startbild | In Listen und Menüs |
|---|---|---|
| **A** (groß, vorn) | Reset. Zweimal kurz hintereinander = Reboot | Eine Ebene zurück |
| **B** (seitlich) | Ultimate-Menü öffnen | Nächster Eintrag |
| **Power** (kurz) | Menü öffnen | Auswählen / ausführen |

Die Power-Taste **lange** gedrückt schaltet den Stick selbst aus – das ist eine
Funktion der Hardware und hat mit dem C64 nichts zu tun.

## Mit MiniJoyC

Steckt ein MiniJoyC, geht es bequemer:

| Eingabe | Auf dem Startbild | In Listen und Menüs |
|---|---|---|
| **Links** | – | Eine Ebene zurück |
| **Rechts** | Menü öffnen | Auswählen / ausführen |
| **Hoch** | Reset | Voriger Eintrag |
| **Runter** | Ausschalten (mit Abfrage) | Nächster Eintrag |
| **Stick drücken** | Ultimate-Menü | Ultimate-Menü |

Wird der MiniJoyC im Betrieb abgezogen, meldet der Stick *MINIJOYC OFFLINE* und
macht mit den Tasten weiter.

# Das Startbild

Das Startbild zeigt das 1MHz-Logo – wahlweise ruhig oder mit wechselnden
Effekten (*Water*, *RotoZoom*, *SineWave*, *Ripple*, *Raster*). Welche Effekte
laufen und wie lange, stellst du unter *Settings* ein.

Steht *Auto-NFC* nicht auf *Off*, fragt der Stick hier im Hintergrund den
Kartenleser ab. Eine aufgelegte Karte wird also sofort erkannt, ohne dass du
vorher irgendwo hineingehen musst – der bequemste Weg im Alltag.

# Das Menü

Ein Druck auf die Power-Taste (oder Joystick rechts) öffnet das Menü:

| Eintrag | Was er tut |
|---|---|
| **PowerOff** | C64 ausschalten – fragt einmal nach |
| **CPU Speed** | Taktstufe wählen, die Liste kommt vom c64u |
| **NFC / RFID** | Karten lesen und beschreiben |
| **WLAN** | Netze einrichten und verwalten |
| **Joystick Swap** | Joystickports am C64 tauschen (Normal ↔ Swapped) |
| **Status** | WLAN, Erreichbarkeit, Passwort, CPU, Adresse |
| **Settings** | alle Einstellungen |

Bei **PowerOff** erscheint *POWER OFF? NOCHMAL!* – ein zweiter Druck innerhalb
von zwei Sekunden schaltet wirklich aus, alles andere bricht ab.

# CPU-Geschwindigkeit ändern

Unter *CPU Speed* steht oben der aktuelle Wert, darunter die Liste der Stufen,
die dein c64u tatsächlich anbietet. Der Stick fragt sie beim Öffnen ab; ist er
gerade nicht verbunden, zeigt er eine Ersatzliste an.

Auswählen und ausführen – der neue Wert wird kurz eingeblendet und oben
übernommen.

# Joystickports tauschen

Manche Spiele erwarten den Joystick in Port 1, andere in Port 2. Statt das Kabel
umzustecken, lässt sich die Belegung im C64 vertauschen.

Im Menü **Joystick Swap** wählen – jeder Druck schaltet zwischen *Normal* und
*Swapped* hin und her, kurz erscheint *JOY Swapped* bzw. *JOY Normal*.

Unter *Settings → c64u Joystick* steht der aktuelle Stand, und dort schaltest du
durch alle Werte, die dein C64 anbietet: neben *Normal* und *Swapped* je nach
Firmware auch *WASD P1* und *WASD P2* – dann steuert die Tastatur den Port.

Gemeint ist hier die Belegung **im C64**, nicht der MiniJoyC am Stick. Einen
eigenen Fernsteuerbefehl gibt es dafür in der Ultimate-Firmware nicht; der Stick
setzt die Einstellung *Joystick Swapper* in der C64-Konfiguration, genau wie die
Taktstufe. Der Stand bleibt deshalb erhalten, bis er wieder geändert wird.

# WLAN einrichten

Der Stick merkt sich **bis zu vier Netze**. Beim Einschalten sucht er kurz und
verbindet sich mit dem am besten empfangenen bekannten Netz. Klappt das nicht,
probiert er im laufenden Betrieb der Reihe nach die übrigen.

## Weg 1: NFC-Karte auflegen

Das ist der schnellste Weg und der eigentliche Grund für den RFID2 am Stick.

Eine WLAN-Karte legst du am bequemsten am **M5Dial** oder am **M5Stack Core**
an (dort: *WLAN → Auf NFC-Karte*). Diese Karte hältst du dann an den Stick –
fertig.

Zwei Möglichkeiten:

- **Auf dem Startbild**, wenn *Auto-NFC* eingeschaltet ist: einfach auflegen.
- Über *NFC / RFID → Karte lesen* oder *WLAN → Von NFC-Karte*.

Der Stick übernimmt Netz und Passwort, speichert beides und verbindet sich
sofort. Er wartet dabei bis zu acht Sekunden auf das Ergebnis und meldet
*WLAN AKTIV* mit seiner IP-Adresse oder *NETZ NICHT DA*. Ist er mit genau
diesem Netz bereits verbunden, meldet er *SCHON VERBUNDEN* und lässt alles wie
es ist – eine liegen gebliebene Karte richtet also keinen Schaden an.

## Weg 2: Setup-Portal

Ohne Karte geht es über einen eigenen Accesspoint:

*WLAN → Setup-Portal*

Der Stick zeigt dann:

| | |
|---|---|
| **Netz** | `C64uRemote-Setup` |
| **Passwort** | `c64ultimate` |
| **Browser** | `192.168.4.1` |

Mit Handy oder Notebook in dieses Netz gehen, die Adresse im Browser öffnen und
das Formular ausfüllen: WLAN-Name, WLAN-Passwort, Adresse des c64u und – falls
gesetzt – dessen Passwort. Nach *Speichern* schaltet der Stick den Accesspoint
ab und verbindet sich mit dem neuen Netz. Dass die Browserverbindung dabei
abreißt, ist normal.

Ein leer gelassenes Passwortfeld lässt den bisherigen Wert unverändert – so
kannst du die C64-Adresse ändern, ohne das WLAN-Passwort erneut zu tippen.

Wird das Portal fünf Minuten lang nicht benutzt, beendet es sich von selbst.
Ein Druck auf **A** beendet es sofort.

## Netze verwalten

| Eintrag | Was er tut |
|---|---|
| **Gespeichert** | Liste aller Netze. Auswählen verbindet mit diesem Netz |
| **Auf NFC-Karte** | schreibt den zuletzt benutzten Zugang auf eine Karte |
| **Netz löschen** | ein einzelnes Netz aus der Liste werfen |
| **Alle löschen** | die ganze Liste leeren |

In der Liste steht *aktiv* neben dem Netz, mit dem der Stick gerade verbunden
ist. Kommt ein fünftes Netz dazu, fällt das am längsten nicht benutzte heraus.

# NFC-Karten

Alles rund um Karten steht unter *NFC / RFID*:

| Eintrag | Was er tut |
|---|---|
| **Karte lesen** | Karte auflegen, der Inhalt wird ausgeführt |
| **CMD-Karte** | Befehl auswählen und auf eine Karte schreiben |
| **WLAN auf Karte** | den zuletzt benutzten WLAN-Zugang auf eine Karte schreiben |

Fehlt der Leser, steht hinter allen drei Einträgen *kein NFC* und ein Druck
meldet *KEIN RFID2*.

## Karte lesen

Der Stick erkennt selbst, was auf der Karte steht:

- **WLAN-Karte** → Netz übernehmen und verbinden
- **Befehlskarte** → Befehl sofort ausführen
- **Spielekarte** → Hinweis *BRAUCHT SD-KARTE*

Am bequemsten ist es, *Auto-NFC* einzuschalten und die Karte einfach auf dem
Startbild aufzulegen.

## Befehlskarten

Auf einer Karte kann statt eines Spiels auch ein Befehl für den C64 stehen. Eine
solche Karte ist im Alltag erstaunlich praktisch: eine Karte „Reset", eine Karte
„Ultimate-Menü", eine Karte „aus".

Möglich sind:

| Befehl | Wirkung |
|---|---|
| **Reset** | C64 zurücksetzen |
| **Reboot** | Kaltstart |
| **Ultimate Menu** | Ultimate-Menü öffnen |
| **PowerOff direkt** | sofort ausschalten, ohne Nachfrage |
| **PowerOff mit Abfrage** | ausschalten, aber erst nach Bestätigung |
| **CPU x MHz** | Taktstufe setzen |
| **Joystick tauschen** | Joystickports umschalten (Normal ↔ Swapped) |
| **Joystick Normal / Swapped / WASD P1 / WASD P2** | Portbelegung fest setzen |

## Eine Befehlskarte anlegen

1. *NFC / RFID → CMD-Karte* wählen.
2. Aus der Liste den gewünschten Befehl aussuchen. Die Joystick-Belegungen und
   die CPU-Stufen holt der Stick vorher beim c64u ab; rechts steht *JOY* oder
   *CPU*.
3. Karte auflegen. *KARTE OK* bedeutet: geschrieben und zur Kontrolle wieder
   eingelesen.

Die Karte lässt sich beliebig oft neu beschreiben. Der Inhalt bleibt ein
gewöhnlicher NDEF-Textrecord – jede NFC-App auf dem Handy kann so eine Karte
lesen und anzeigen.

## PowerOff mit Abfrage

Eine Karte, die den C64 sofort ausschaltet, kann unangenehm werden, wenn sie
versehentlich auf dem Leser landet. Deshalb gibt es die Variante mit Abfrage:

- Karte auflegen → *POWER OFF? NOCHMAL!*
- Innerhalb des Zeitfensters **dieselbe Karte noch einmal auflegen** oder eine
  Taste drücken → der C64 geht aus
- Nichts tun → nichts passiert

Wie lang das Fenster ist, stellst du unter *Settings → NFC-Cmd PowOff* ein
(3, 5, 8 oder 15 Sekunden, Werkseinstellung 8 s). Die Zeit wird beim Anlegen
der Karte fest mitgeschrieben.

## Was auf der Karte steht

Für alle, die Karten lieber mit einer Handy-App schreiben: Der Text ist
schlicht lesbar.

```
CMD:RESET
CMD:REBOOT
CMD:MENU
CMD:POWEROFF=0      sofort ausschalten
CMD:POWEROFF=8      nachfragen, 8 s Zeit für die Bestätigung
CMD:CPU=10          CPU auf 10 MHz stellen
CMD:JOY             Joystickports umschalten
CMD:JOY=SWAPPED     Ports fest setzen; auch NORMAL, WASD1, WASD2
```

Groß- und Kleinschreibung sowie Leerzeichen sind egal. Dasselbe Format benutzen
M5Dial und M5Stack Core – eine Karte funktioniert an allen Geräten.

WLAN-Karten benutzen das Schema aus dem WLAN-QR-Code:

```
WIFI:S:MeinNetz;T:WPA;P:geheim;;
```

## Warum keine Spielekarten

Eine Spielekarte enthält den **Pfad einer Programmdatei**. Damit daraus ein
laufendes Spiel wird, muss das Gerät die Datei von einer eigenen microSD-Karte
holen und an den C64 schicken. Der M5StickC Plus2 hat keinen Kartenschacht –
deshalb meldet er bei so einer Karte *BRAUCHT SD-KARTE*.

Die Karte bleibt dabei völlig unangetastet und gültig; sie gehört an den M5Dial
oder den M5Stack Core. Umgekehrt gilt: Befehls- und WLAN-Karten laufen an allen
vier Geräten gleich.

# Einstellungen im Überblick

Alle Einstellungen werden sofort gespeichert und überstehen einen Neustart.

## NFC

| Eintrag | Auswahl |
|---|---|
| **Auto-NFC** | Abstand der Hintergrundabfrage auf dem Startbild: *Off*, *1.5s*, *0.7s*, *0.3s* |
| **NFC-Cmd PowOff** | Vorgabe für die Abfragezeit einer PowerOff-Befehlskarte: 3, 5, 8, 15 s |

Ein kürzerer Abstand reagiert schneller, kostet aber etwas mehr Strom. *Off*
schaltet die Hintergrundabfrage ganz ab; Karten gehen dann nur noch über
*NFC / RFID → Karte lesen*.

## Anzeige

| Eintrag | Auswahl |
|---|---|
| **Animations** | Effekte auf dem Startbild ein- oder ausschalten |
| **Effect** | *Auto* oder ein fester Effekt |
| **Anim Speed** | *Slow*, *Normal*, *Fast* |
| **Effect Time** | wie lange ein Effekt läuft |
| **Static Time** | wie lange das ruhige Logo dazwischen steht |
| **Brightness** | Helligkeit des Displays |

## MiniJoyC

| Eintrag | Auswahl |
|---|---|
| **Joy Overlay** | kleine Anzeige der Joystick-Werte ein/aus |
| **Joy LED** | Status-LED ein/aus |
| **Joy LED Bright** | Helligkeit der LED |
| **Joy Threshold** | ab welchem Ausschlag eine Richtung zählt (40–100) |

## Sonstiges

| Eintrag | Auswahl |
|---|---|
| **c64u Joystick** | Portbelegung im C64: *Normal*, *Swapped*, je nach Firmware *WASD P1* / *WASD P2* |
| **Factory Reset** | alle Einstellungen auf die Werkswerte zurück |

*Factory Reset* betrifft nur die Einstellungen. Gespeicherte WLAN-Zugänge
bleiben erhalten – die wirft man unter *WLAN → Alle löschen* weg.

# Häufige Fragen

**Der Bildschirm bleibt dunkel.**
Das ist fast immer die Board-Einstellung beim Übersetzen, nicht die Hardware.
Siehe technische Dokumentation, Kapitel *Programmierumgebung*.

**Der Stick findet den C64 nicht.**
*Status* prüfen. Steht dort *Disconnected*, fehlt das WLAN. Steht *Not reached*,
stimmt die Adresse nicht oder der C64 ist aus. Steht *Auth failed*, ist das
Passwort des c64u falsch.

**Nach dem Auflegen einer WLAN-Karte kommt *NETZ NICHT DA*.**
Der Zugang ist trotzdem gespeichert. Der Stick versucht es im Hintergrund
weiter; oft ist das Netz nur langsamer als die acht Sekunden Wartezeit.

**Eine Karte wird nicht erkannt.**
Karte etwas anders auflegen – die Antenne des RFID2 sitzt mittig unter dem
Gehäuse. Steht unter *NFC / RFID* überall *kein NFC*, wurde der Leser beim Start
nicht gefunden: Stecker prüfen und den Stick neu starten.

**Die gleiche Karte löst dauernd wieder aus.**
Tut sie nicht: eine liegen gebliebene Karte wird erst nach zweieinhalb Sekunden
erneut angenommen. Nur die Rückfrage einer PowerOff-Karte darf sofort bestätigt
werden.

**Kann ich Karten am Stick anlegen und am M5Dial benutzen?**
Ja. Befehls- und WLAN-Karten sind zwischen allen vier Geräten austauschbar.

# Lizenz

C64uRemote steht unter der **MIT-Lizenz**. Der vollständige Lizenztext liegt als
Datei `LICENSE` im Projektstamm.

Ursprung ist das Projekt **C64uRemote von Karl Prosser (@klumsy)**,
<https://github.com/ReadyOS-C64/C64uRemote>, das er unter der MIT-Lizenz
veröffentlicht hat. Diese Fassung ist eine daraus abgeleitete Erweiterung und
steht unter denselben Bedingungen:

* Copyright (c) 2026 Karl Prosser – Originalprojekt
* Copyright (c) 2026 Martin Oswald (@mad, <https://1MHz.de>) – Portierung und Erweiterungen

Die MIT-Lizenz erlaubt es, die Software zu benutzen, zu verändern und
weiterzugeben, auch kommerziell. Einzige Bedingung: **Copyright-Vermerk und
Lizenztext müssen erhalten bleiben** und jeder Kopie beiliegen. Eine
Gewährleistung oder Haftung ist ausgeschlossen.

Die eingebundenen Bibliotheken haben ihre eigenen Lizenzen: M5Unified und M5GFX
(MIT, © M5Stack), ArduinoJson (MIT, © Benoit Blanchon) sowie MFRC522_I2C
(<https://github.com/kkloesener/MFRC522_I2C>). Sie werden beim Bauen von
PlatformIO geladen.

# Entstehung

Portierung, Erweiterungen und Handbücher sind mit Unterstützung von Claude
(Anthropic) entstanden. Konzept, Idee, Hardware-Entscheidungen und sämtliche
Tests auf den echten Geräten: Martin Oswald (@mad).
