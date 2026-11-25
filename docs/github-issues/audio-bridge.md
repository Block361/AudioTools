# Audio Bridge

Stand: OpenAoIP 0.6.3. Die Bridge verbindet vorhandene Windows-Audio-Endpunkte mit dem
OpenAoIP-Netzwerk und arbeitet vollständig im User Mode.

## Geräte hinzufügen

1. **Add device** wählen und einen kurzen Anzeigenamen vergeben.
2. Richtung auswählen:
   - **Capture → TX**: Mikrofon, Soundkarte, USB-Interface oder Aufnahme-Endpunkt eines virtuellen Kabels.
   - **Loopback → TX**: Windows-Mix eines ausgewählten Wiedergabegeräts, z. B. Browser/Systemaudio.
   - **RX → Playback**: Empfang auf einem lokalen Wiedergabegerät.
3. Ein tatsächlich vorhandenes Gerät auswählen. Ein virtuelles Audiokabel muss separat installiert sein.
4. Gleich lange Listen für lokale und Netzwerkkanäle eintragen und **Apply** wählen.

| Zweck | Local channels | Network channels |
|---|---|---|
| Stereo auf TX 1/2 | `1, 2` | `1, 2` |
| Stereo auf TX 3/4 | `1, 2` | `3, 4` |
| Mono auf TX 1 | `1` | `1` |
| Mono auf TX 1/2 duplizieren | `1, 1` | `1, 2` |
| RX 3/4 auf lokale Ausgänge 1/2 | `1, 2` | `3, 4` bei Playback |

Alle Kanalzuordnungen eines Geräts und einer Richtung in einem Eintrag zusammenfassen.
Jeder TX hat genau eine lokale Quelle. Jeder lokale Wiedergabekanal wird einmal belegt;
die Bridge summiert nicht mehrere RX zu einem Ausgang.

## Start und Kontrolle

Eindeutigen Gerätenamen, LAN-Schnittstelle, Controller, Kanalzahlen, Netzwerk-Samplerate und
Puffer konfigurieren. **Start** meldet die Bridge beim Controller an; die gewünschten Routen
werden dort verbunden. Es werden beim Start keine neuen Routen automatisch angelegt.

**Network channels** zeigt Pegel und erlaubt bei gestoppter Bridge das Benennen der Netzwerkkanäle.
**Diagnostics** zeigt native Formate, Puffer, Queue, Driftkorrektur und Underruns.
Konfiguration wird gespeichert; **Save config / Load config** dient dem Export/Import.

Ausgewählte Geräte werden über ihre Windows-Endpunkt-ID wieder geöffnet. Ein fehlendes
Gerät wird als Fehler angezeigt und erneut versucht. **Windows default device** folgt dagegen
dem Standardgerät. Eine höhere Netzwerk-Kanalzahl erweitert nicht die physischen WASAPI-Kanäle.

Serato-Beispiel: **Serato → Virtual Audio Cable → WASAPI Capture → TX 1/2 → Netzwerk**.
Die tatsächliche Serato-/Kabel-Konfiguration und Kompatibilität muss auf dem jeweiligen PC geprüft werden.
Bei lokalem Mikrofon-Monitoring Kopfhörer verwenden, um Rückkopplung zu vermeiden.

## Weitere Dokumentation

[PTP](./ptp.md)

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Controller](./controller.md) ·
[Virtual Soundcard](./virtual-soundcard.md) · [Puffer und Latenz](./puffer-latenz.md) ·
[Audioqualität](./audioqualitaet.md) · [Recovery](./recovery.md) · [Diagnose](./diagnose.md)
