# OpenAoIP für Windows – Version 0.6.0

Drei eigenständige native Anwendungen: **Controller**, **Virtual Soundcard** und **Audio Bridge**.
Sie übertragen tatsächlich Audio über RTP. Dies ist weiterhin ein Entwicklungsprototyp:
PTP, samplegenaue Synchronisation zwischen PCs und Dante-/AES67-Interoperabilität fehlen.
Der WDM-Kerneltreiber ist experimentell und besitzt keine Produktionsfreigabe.

## Updates installieren

1. Audio stoppen. Alle OpenAoIP-Anwendungen und alle Programme schließen, die den OpenAoIP-ASIO-Treiber verwenden.
   Die Hintergrund-Engine beendet sich, sobald die letzte Audio-Anwendung geschlossen ist und keine andere
   Audiositzung läuft. Nach einem Absturz kann sie bei Inaktivität noch bis zu einer Minute laufen.
2. **OpenAoIPSuiteSetup.exe** für alle drei Anwendungen ausführen. Alternativ einzeln:
   **OpenAoIPControllerSetup.exe**, **OpenAoIPVirtualSoundcardSetup.exe**, **OpenAoIPAudioBridgeSetup.exe**.
3. Im Startmenü unter **OpenAoIP** öffnen. Oben links muss **0.6.0** stehen.

Erneutes Ausführen desselben Setup-Typs aktualisiert die vorhandene Installation pro Benutzer.
Einstellungen und Routing liegen außerhalb des Programmordners und bleiben erhalten.
Suite und Einzel-Setups sind getrennte Installationen: Vor einem Wechsel die alten Apps
über Windows „Installierte Apps“ entfernen und dann die Suite bzw. die Einzel-Apps installieren.

Nach einem Wechsel des Installationsordners in der Virtual Soundcard **Register ASIO** wählen.
Damit zeigt die ASIO-Registrierung auf die neue DLL. Beim Update im selben Ordner entfällt dies.
ASIO benötigt keinen Windows-Testmodus; seine bewusste Registrierung benötigt Administratorrechte.
Die Installation der Apps selbst benötigt keine Administratorrechte.

Die Entwicklungs-EXEs sind nicht digital signiert. Reguläre Installer ändern keine Sicherheits-,
Boot- oder Firewall-Einstellungen und installieren keinen WDM-Kerneltreiber.
WDM wird gesondert installiert: [Windows-Audio](./WINDOWS-AUDIO.md), [VM-Test](./drivers/wdm/VM-TEST.md).

## Controller

1. Einen Controller im kabelgebundenen Audio-LAN starten. Einen alten Web-Controller auf TCP 3210
   vorher beenden; das Schließen seines Browserfensters beendet den Server nicht.
   Alternativ die native Anwendung mit `--port 3214` starten.
2. Auf jedem Audio-PC Soundcard oder Bridge konfigurieren und **Start** wählen.
   Automatische Erkennung gilt im lokalen Broadcast-Netz. Alternativ eine Controller-URL
   eingeben, etwa `http://192.168.1.10:3210`.
3. **Routing**: TX horizontal, RX vertikal. Geräte über **+ / −** aufklappen.
   Ein Klick verbindet genau einen TX-Kanal mit einem RX-Kanal; derselbe Klick entfernt die Route.
   Ein Stereo-Signal benötigt zwei Einzelkanalrouten. Fan-out ist möglich.
4. Suchfelder filtern Geräte/Kanalnamen, **Online only** filtert Offline-Geräte.
   Doppelklick auf Geräte-/Kanalnamen ändert den gespeicherten Controller-Alias.
5. **Save preset / Load preset** speichert bzw. ersetzt Routen und Aliasse atomar.
   Ungültige Presets werden vollständig abgelehnt; fehlende Geräte bleiben ausstehend.

Grün = Empfang bestätigt, Gelb = ausstehend/puffert, Rot = keine Pakete/Fehler, Grau = offline/inaktiv.
Paketempfang bestätigt keine physische Lautsprecherausgabe. Die Tabs **Device Info**, **Clock Status**,
**Network Status** und **Events** zeigen tatsächliche Zustände. Kanalzahl/Format am Audio-PC einstellen.
Routen werden automatisch gespeichert. Audio fließt direkt zwischen Endpunkten und kann mit
zuletzt angewendetem Routing bei einem Controller-Ausfall weiterlaufen.

## Virtual Soundcard

1. **ASIO x64**, eindeutigen Namen, LAN-Schnittstelle, TX/RX-Anzahl und Samplerate wählen.
   Unterstützt: 1–32 Kanäle je Richtung bei 44,1/48/96 kHz; bevorzugter Puffer 64–1024 Frames.
   Der ASIO-Host handelt den tatsächlichen Puffer aus; dieser steht unter **Host buffer**.
2. **Internal / free running**, zunächst 60 oder 100 ms Netzwerklatenz und **Start** wählen.
3. Die DAW öffnen und **OpenAoIP Virtual Soundcard** als x64-ASIO-Treiber wählen.
   Programm-Ausgänge werden Netzwerk-TX; Netzwerk-RX wird zu Programm-Eingängen.
   Nur ein ASIO-Host kann die Soundcard gleichzeitig belegen.
4. Im Controller Kanäle verbinden. Format-/Pufferänderungen nur bei gestoppter Soundcard und
   geschlossenem ASIO-Host vornehmen. **Stop** oder Schließen beendet ihre Audiositzung.

**WDM** ist alternativ nur nutzbar, wenn Windows einen kompatiblen Treiber tatsächlich geladen
und alle 16 Stereo-Paare je Richtung bereitgestellt hat. Dann gelten feste 48 kHz.
Ohne akzeptierten Treiber verweigert die Anwendung den WDM-Start. Das WDM-Testpaket bleibt
separat in Version 0.5; der unveränderte Treiber verwendet weiterhin Protokoll v2.

## Audio Bridge

Die Bridge arbeitet vollständig im User Mode und benötigt keinen OpenAoIP-Kerneltreiber.

1. **Add device**: Anzeigename und Richtung wählen: **WASAPI capture → network TX**,
   **WASAPI loopback → network TX** oder **Network RX → WASAPI playback**.
2. Das tatsächlich vorhandene Windows-Gerät auswählen. Loopback verwendet ein Wiedergabegerät
   und erfasst dessen Windows-Mix, etwa Browser-/Multimedia-Audio.
3. Gleich lange Kanalnummernlisten eintragen. Stereo: lokal `1, 2` → Netzwerk `3, 4`.
   Mono: `1` → `1`; Mono auf zwei TX-Kanäle: lokal `1, 1` → Netzwerk `1, 2`.
   Bei Playback bezeichnet „Network channels“ die RX-Eingänge, „Local channels“ die Ausgänge.
4. Weitere Quellen/Ausgänge hinzufügen. Alle Zuordnungen eines Geräts und einer Richtung
   in einem Eintrag zusammenfassen. Jeder TX-Kanal hat genau eine Quelle; ein lokaler
   Wiedergabekanal wird einmal zugewiesen. Es gibt keinen automatischen Audiomischer.
5. Netzwerk, Kanalzahl, Format und Puffer wählen, dann **Start**. Im Controller routen.
6. **Network channels** zeigt gemessene Pegel; bei gestoppter Bridge per Doppelklick umbenennen.
   **Diagnostics** zeigt native Formate/Puffer, Queue, Driftkorrektur und Underruns.

Serato-Beispiel: Serato → bereits installiertes Virtual Audio Cable → dessen Aufnahme-Endpunkt
in WASAPI Capture → TX 1/2 → Netzwerk. Das Kabel ist separate Software und wird nicht mitinstalliert.

Konfigurationen werden gespeichert; **Save config / Load config** exportiert bzw. lädt Dateien.
Ausgewählte Geräte werden über stabile Windows-IDs wiedergefunden und nach Verlust erneut geöffnet.
**Windows default device** folgt dagegen dem Standardgerät. Fehler werden angezeigt, keine Geräte simuliert.
Beim Start werden keine Routen angelegt. Kopfhörer bei lokalem Mikrofon-Monitoring verwenden.

Samplerate-Konvertierung und ein adaptiver lokaler Puffer kompensieren Restdrift. Netzwerk- und
lokaler Puffer addieren sich; ihre Werte sind keine gemessene Ende-zu-Ende-Latenz. Dies ersetzt
keine PTP-Clock und garantiert keine Phasenkohärenz. PTP wird ausdrücklich als nicht verfügbar angezeigt.

## Netzwerk, Engine und Einstellungen

UDP 32100: Erkennung; TCP 3210 bzw. gewählter Port: Steuerung; dynamische UDP-Ports: Audio.
In der privaten Windows-Firewall bei Bedarf **Controller** und **OpenAoIP Engine** für das Audio-LAN
zulassen. Außerhalb des Broadcast-Netzes eine direkt erreichbare Controller-URL verwenden.
Die LAN-Steuerung besitzt noch keine Benutzeranmeldung/Verschlüsselung und gehört in ein vertrauenswürdiges LAN.

Eine gemeinsame Engine läuft pro Windows-Benutzersitzung und wird bei Bedarf gestartet.
Soundcard und Bridge können gleichzeitig laufen. Es wird kein SCM-Dienst installiert.
Die lokale API bindet nur an `127.0.0.1` und benötigt einen zufälligen Token; diesen nicht weitergeben.
Einstellungen: `%LOCALAPPDATA%/OpenAoIP/Controller`, `/VirtualSoundcard` und `/AudioBridge`.
[Architektur und Testgrenzen](./ARCHITECTURE.md).

## Selbst bauen

Visual Studio mit Desktopentwicklung mit C++, Windows SDK und CMake:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1 -BuildDirectory build/desktop -Test -Package
```

Programme und Setup-EXEs: `build/desktop/Release`; einzelne ZIPs und Suite: `build/desktop/packages`.
CLI/Webversion bleiben erhalten. ASIO und dessen mitgelieferter zugehöriger Quelltext: GPL-3.0-only;
unabhängige Anwendungen: MIT. Ein übersprungener WDM-Runtime-Test bedeutet **nicht getestet**.
