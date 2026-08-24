% C64uRemote for the M5StickC Plus2
% User Manual
% Version 1.0

# Welcome

C64uRemote turns an **M5StickC Plus2** into a pocket-sized remote control for
the **Commodore 64 Ultimate (c64u)** and the **Ultimate64 Elite-II**. Over
Wi-Fi it drives the machine: reset, reboot, power off, open the Ultimate menu
and change the CPU speed.

The optional **Unit RFID2** on the Grove port adds the thing that makes the
little stick genuinely practical: you set up its **Wi-Fi with an NFC card**. A
card written on the M5Dial or the M5Stack Core goes onto the reader, and the
stick takes over network and password — it never has to go back to a computer.
The same **command cards** as on the other devices work here too, and the stick
can write cards itself.

What it cannot do is start games from a card — it has no memory card. See the
*NFC cards* chapter.

This project builds on the original idea by Karl Prosser (@klumsy) and was
extended for the M5StickC Plus2 by Martin Oswald (@mad, 1MHz.de).

# What you need

**Required:**

- An M5StickC Plus2
- A Commodore 64 Ultimate or Ultimate64 Elite-II on the same Wi-Fi

**Optional, for NFC cards:**

- A **Unit RFID2** (WS1850S) on the Grove port (HY2.0-4P)
- NFC cards: NTAG215 recommended, NTAG213/216 and MIFARE Classic also work

**Optional, for input:**

- A **Hat MiniJoyC** on the HAT port

Both add-ons are detected at power-up. If one is missing, everything else keeps
working. RFID2 and MiniJoyC sit on separate connectors and do not interfere, so
you can use both at once.

# First setup

Before the stick can find your C64, the Wi-Fi credentials and the address of
the C64 have to be stored once. There are two ways to do that, and neither
needs **reflashing**:

1. Present a **Wi-Fi card** (requires the RFID2)
2. Start the **setup portal** and fill in a form in your browser

Both are covered in the *Setting up Wi-Fi* chapter. The data goes into internal
storage and survives every restart.

If you prefer to supply the credentials at build time, put them into
`build_env.h` (see the technical documentation). They then act as the initial
values for the very first start and are overridden by anything you set on the
device later.

On the first start with no network stored the stick reports **WLAN
EINRICHTEN** (set up Wi-Fi). If the network is known but the C64 address is
missing, it reports **HOST FEHLT** (host missing).

## The MiniJoyC status LED

With a MiniJoyC attached, its RGB LED shows the overall state without opening
any menu:

| LED | Meaning |
|---|---|
| **green** | connected, C64 answers, password accepted |
| **blue** | Wi-Fi up, but the C64 does not answer |
| **blue, blinking** | C64 reachable, but the password is wrong |
| **red** | no Wi-Fi connection |

Brightness and on/off are in *Settings*.

# Operation

The M5StickC Plus2 has three buttons: the **large button on the front** (A),
the **small button on the side** (B) and the **power button** on the left.

| Button | On the home screen | In lists and menus |
|---|---|---|
| **A** (large, front) | Reset. Twice in quick succession = reboot | One level back |
| **B** (side) | Open the Ultimate menu | Next entry |
| **Power** (short) | Open the menu | Select / execute |

Holding the power button switches the stick itself off — that is a hardware
function and has nothing to do with the C64.

## With a MiniJoyC

With a MiniJoyC attached it gets more comfortable:

| Input | On the home screen | In lists and menus |
|---|---|---|
| **Left** | – | One level back |
| **Right** | Open the menu | Select / execute |
| **Up** | Reset | Previous entry |
| **Down** | Power off (asks first) | Next entry |
| **Press stick** | Ultimate menu | Ultimate menu |

If the MiniJoyC is unplugged while running, the stick reports *MINIJOYC
OFFLINE* and carries on with the buttons.

# The home screen

The home screen shows the 1MHz logo — either calm or with changing effects
(*Water*, *RotoZoom*, *SineWave*, *Ripple*, *Raster*). Which effects run and
for how long is set in *Settings*.

Unless *Auto-NFC* is *Off*, the stick polls the card reader in the background
here. A card is picked up straight away, without going into any menu first —
the most convenient way in daily use.

# The menu

A press of the power button (or joystick right) opens the menu:

| Entry | What it does |
|---|---|
| **PowerOff** | switch the C64 off — asks once |
| **CPU Speed** | pick a clock speed, the list comes from the c64u |
| **NFC / RFID** | read and write cards |
| **WLAN** | set up and manage networks |
| **Connection Test** | check the connection and show the result |
| **Status** | Wi-Fi, reachability, password, CPU, address |
| **Settings** | all settings |

**PowerOff** shows *POWER OFF? NOCHMAL!* — a second press within two seconds
really switches off, anything else cancels.

# Changing the CPU speed

*CPU Speed* shows the current value at the top and below it the list of steps
your c64u actually offers. The stick queries them when the screen opens; if it
is not connected at that moment, it shows a fallback list.

Select and execute — the new value is shown briefly and taken over at the top.

# Setting up Wi-Fi

The stick remembers **up to four networks**. At power-up it scans briefly and
connects to the known network with the best signal. If that fails, it works
through the remaining ones while running.

## Way 1: present an NFC card

This is the quickest route, and the actual reason for putting an RFID2 on the
stick.

A Wi-Fi card is most easily created on the **M5Dial** or the **M5Stack Core**
(there: *WLAN → Auf NFC-Karte*). Hold that card against the stick — done.

Two options:

- **On the home screen**, if *Auto-NFC* is on: simply present the card.
- Via *NFC / RFID → Karte lesen* or *WLAN → Von NFC-Karte*.

The stick takes over network and password, stores both and connects
immediately. It waits up to eight seconds for the result and reports *WLAN
AKTIV* together with its IP address, or *NETZ NICHT DA*. If it is already
connected to exactly that network it reports *SCHON VERBUNDEN* and leaves
everything alone — so a card left lying on the reader does no harm.

## Way 2: the setup portal

Without a card it works through a private access point:

*WLAN → Setup-Portal*

The stick then shows:

| | |
|---|---|
| **Network** | `C64uRemote-Setup` |
| **Password** | `c64ultimate` |
| **Browser** | `192.168.4.1` |

Join that network with a phone or laptop, open the address in a browser and
fill in the form: Wi-Fi name, Wi-Fi password, the address of the c64u and — if
set — its password. After *Speichern* (save) the stick shuts the access point
down and connects to the new network. The browser connection breaking off in
the process is normal.

A password field left empty keeps the previous value — so you can change the
C64 address without retyping the Wi-Fi password.

If the portal is not used for five minutes it closes by itself. A press of
**A** closes it immediately.

## Managing networks

| Entry | What it does |
|---|---|
| **Gespeichert** | list of all networks. Selecting one connects to it |
| **Auf NFC-Karte** | writes the most recently used credentials to a card |
| **Netz loeschen** | drop a single network from the list |
| **Alle loeschen** | clear the whole list |

In the list, *aktiv* marks the network the stick is currently connected to. When
a fifth network is added, the least recently used one drops out.

# NFC cards

Everything about cards lives under *NFC / RFID*:

| Entry | What it does |
|---|---|
| **Karte lesen** | present a card, its content is executed |
| **CMD-Karte** | pick a command and write it to a card |
| **WLAN auf Karte** | write the most recently used Wi-Fi credentials to a card |

Without a reader all three show *kein NFC* and a press reports *KEIN RFID2*.

## Reading a card

The stick works out for itself what is on the card:

- **Wi-Fi card** → take over the network and connect
- **Command card** → run the command right away
- **Game card** → the notice *BRAUCHT SD-KARTE* (needs an SD card)

The most comfortable way is to switch *Auto-NFC* on and present the card on the
home screen.

## Command cards

Instead of a game, a card can carry a command for the C64. Such a card is
surprisingly handy in daily use: one card for "reset", one for "Ultimate menu",
one for "off".

Available commands:

| Command | Effect |
|---|---|
| **Reset** | reset the C64 |
| **Reboot** | cold start |
| **Ultimate Menu** | open the Ultimate menu |
| **PowerOff direkt** | switch off immediately, no prompt |
| **PowerOff with prompt** | switch off, but only after confirmation |
| **CPU x MHz** | set a clock speed |

## Creating a command card

1. Choose *NFC / RFID → CMD-Karte*.
2. Pick the command from the list. The stick fetches the CPU steps from the
   c64u beforehand.
3. Present a card. *KARTE OK* means: written and read back for verification.

A card can be rewritten as often as you like. The content stays a plain NDEF
text record — any NFC app on a phone can read and display such a card.

## PowerOff with a prompt

A card that switches the C64 off immediately can be unpleasant if it ends up on
the reader by accident. Hence the variant with a prompt:

- Present the card → *POWER OFF? NOCHMAL!*
- Within the time window **present the same card again** or press a button →
  the C64 goes off
- Do nothing → nothing happens

The length of the window is set in *Settings → NFC-Cmd PowOff* (3, 5, 8 or 15
seconds, 8 s by default). The time is written into the card when it is created.

## What is on the card

For anyone who would rather write cards with a phone app: the text is plainly
readable.

```
CMD:RESET
CMD:REBOOT
CMD:MENU
CMD:POWEROFF=0      switch off immediately
CMD:POWEROFF=8      ask first, 8 s to confirm
CMD:CPU=10          set the CPU to 10 MHz
```

Case and spaces do not matter. The M5Dial and M5Stack Core use the same format —
one card works on every device.

Wi-Fi cards use the scheme from the Wi-Fi QR code:

```
WIFI:S:MyNetwork;T:WPA;P:secret;;
```

## Why there are no game cards

A game card holds the **path of a program file**. To turn that into a running
game, the device has to fetch the file from its own microSD card and send it to
the C64. The M5StickC Plus2 has no card slot — so it reports *BRAUCHT
SD-KARTE* for such a card.

The card itself is left completely untouched and stays valid; it belongs on the
M5Dial or the M5Stack Core. The other way round: command and Wi-Fi cards behave
identically on all four devices.

# Settings at a glance

Every setting is saved immediately and survives a restart.

## NFC

| Entry | Choices |
|---|---|
| **Auto-NFC** | background polling interval on the home screen: *Off*, *1.5s*, *0.7s*, *0.3s* |
| **NFC-Cmd PowOff** | default prompt time for a PowerOff command card: 3, 5, 8, 15 s |

A shorter interval reacts faster but draws slightly more power. *Off* disables
background polling entirely; cards then only work through *NFC / RFID → Karte
lesen*.

## Display

| Entry | Choices |
|---|---|
| **Animations** | turn home screen effects on or off |
| **Effect** | *Auto* or one fixed effect |
| **Anim Speed** | *Slow*, *Normal*, *Fast* |
| **Effect Time** | how long an effect runs |
| **Static Time** | how long the calm logo sits in between |
| **Brightness** | display brightness |

## MiniJoyC

| Entry | Choices |
|---|---|
| **Joy Overlay** | small readout of the joystick values on/off |
| **Joy LED** | status LED on/off |
| **Joy LED Bright** | LED brightness |
| **Joy Threshold** | how far the stick must move for a direction to count (40–100) |

## Other

| Entry | Choices |
|---|---|
| **Factory Reset** | all settings back to their defaults |

*Factory Reset* only touches the settings. Stored Wi-Fi credentials survive —
those are cleared under *WLAN → Alle loeschen*.

# Frequently asked questions

**The screen stays dark.**
That is almost always the board setting at build time, not the hardware. See
the technical documentation, chapter *Build environment*.

**The stick does not find the C64.**
Check *Status*. *Disconnected* means no Wi-Fi. *Not reached* means the address
is wrong or the C64 is off. *Auth failed* means the c64u password is wrong.

**After presenting a Wi-Fi card it says *NETZ NICHT DA*.**
The credentials are stored anyway. The stick keeps trying in the background;
often the network is simply slower than the eight second wait.

**A card is not recognised.**
Move the card a little — the RFID2 antenna sits centred under the case. If
*NFC / RFID* shows *kein NFC* everywhere, the reader was not found at startup:
check the plug and restart the stick.

**The same card keeps triggering over and over.**
It does not: a card left on the reader is only accepted again after two and a
half seconds. Only the prompt of a PowerOff card may be confirmed straight
away.

**Can I create cards on the stick and use them on the M5Dial?**
Yes. Command and Wi-Fi cards are interchangeable between all four devices.

# License

C64uRemote is released under the **MIT License**. The full text is in the file
`LICENSE` in the project root.

It originates from **C64uRemote by Karl Prosser (@klumsy)**,
<https://github.com/ReadyOS-C64/C64uRemote>, which he published under the MIT
License. This version is a derivative work and is released under the same terms:

* Copyright (c) 2026 Karl Prosser – original project
* Copyright (c) 2026 Martin Oswald (@mad, <https://1MHz.de>) – port and extensions

The MIT License allows you to use, modify and redistribute the software,
including commercially. The only condition: **the copyright notice and the
license text must be kept** and included with every copy. There is no warranty
and no liability.

The libraries used carry their own licenses: M5Unified and M5GFX (MIT,
© M5Stack), ArduinoJson (MIT, © Benoit Blanchon) and MFRC522_I2C
(<https://github.com/kkloesener/MFRC522_I2C>). PlatformIO fetches them at build
time.

# How this was made

The port, the extensions and these manuals were written with the help of
Claude (Anthropic). Concept, idea, hardware decisions and every test on real
devices: Martin Oswald (@mad).
