# Installation und Updates

Stand: OpenAoIP 0.6.0 für Windows x64. Die lokal gebauten Entwicklungs-Setups sind nicht
digital signiert. Ein GitHub-Release oder automatischer Updater wird nicht vorausgesetzt.

## Varianten

| Setup | Installiert |
|---|---|
| OpenAoIPControllerSetup.exe | Controller |
| OpenAoIPAudioBridgeSetup.exe | Audio Bridge und gemeinsame Engine |
| OpenAoIPVirtualSoundcardSetup.exe | Virtual Soundcard, Engine und ASIO-Komponente |
| OpenAoIPSuiteSetup.exe | Alle drei Anwendungen mit Engine und ASIO-Komponente |

Die Installation erfolgt pro Windows-Benutzer. Anwendungen erscheinen im Startmenü unter
OpenAoIP und lassen sich über Windows „Installierte Apps“ entfernen.

## Update

1. Audio stoppen, OpenAoIP-Anwendungen und Programme mit dem OpenAoIP-ASIO-Treiber schließen.
2. Denselben Setup-Typ erneut ausführen. Einstellungen und Routing liegen außerhalb des
   Programmordners und bleiben erhalten.
3. Im Startmenü öffnen und die angezeigte Versionsnummer prüfen.

Suite und Einzel-Setups sind getrennte Installationen. Bei einem Wechsel zuerst die bisherigen
Apps entfernen und danach die gewünschte Variante installieren. Die Einstellungen bleiben erhalten.
Nach einem Wechsel des Installationsordners ASIO erneut über **Register ASIO** registrieren.
Beim Update im selben Ordner ist dies normalerweise nicht erforderlich.

## Treiber und Engine

ASIO-Registrierung erfordert Administratorrechte, ASIO selbst keinen Windows-Testmodus.
Die Bridge benötigt keinen OpenAoIP-Kerneltreiber. Reguläre Setups installieren keinen WDM-Treiber,
aktivieren keinen Testmodus und ändern keine Windows-Sicherheits- oder Firewall-Einstellungen.

Die Engine wird bei Bedarf gestartet. Beim Schließen der letzten Audio-Anwendung kann sie sich
beenden, wenn keine weitere Sitzung aktiv ist. Bei Inaktivität ohne laufende Audiositzungen
beendet sie sich nach bis zu 60 Sekunden. Laufende Sitzungen werden nicht zwangsweise beendet.

## Netzwerk und Einstellungen

- UDP 32100: Controller-Suche im lokalen Broadcast-Netz.
- TCP 3210 bzw. konfigurierter Port: Anmeldung und Routing.
- Dynamische UDP-Ports: Audio zwischen Endpunkten.
- Bei Bedarf Controller und Engine in der privaten Windows-Firewall für das Audio-LAN freigeben.
- Einstellungsordner: `%LOCALAPPDATA%/OpenAoIP/Controller`, `/AudioBridge`, `/VirtualSoundcard`.

Die LAN-Steuerung besitzt noch keine Anmeldung/Verschlüsselung und gehört in ein vertrauenswürdiges LAN.
Eine direkte Controller-URL verwenden, wenn automatische Suche über Subnetzgrenzen nicht funktioniert.

## Weitere Dokumentation

[Übersicht](./uebersicht.md) · [Controller](./controller.md) · [Audio Bridge](./audio-bridge.md) ·
[Virtual Soundcard](./virtual-soundcard.md) · [Puffer und Latenz](./puffer-latenz.md) ·
[Audioqualität](./audioqualitaet.md) · [Recovery](./recovery.md) · [Diagnose](./diagnose.md)
