# Audioqualität und Dynamik

Stand: OpenAoIP 0.6.0. Im Netzwerk wird **L16/16-Bit-PCM** übertragen. Die Bridge verwendet
lokal Float-Audio und konvertiert für diesen Transport. Eine Float-Schnittstelle zur Soundkarte
macht die Netzwerkübertragung nicht automatisch zu 24 Bit.

## Bittiefe und Samplerate

Bittiefe bestimmt die Auflösung der Sample-Amplitude. Der idealisierte Dynamikumfang beträgt
ungefähr 96 dB bei 16 Bit und 144 dB bei 24 Bit. Nutzbarer Dynamikumfang wird zusätzlich durch
Mikrofon, Soundkarte, analoge Verstärkung und Grundrauschen begrenzt.

24-Bit-Netzwerktransport ist noch nicht implementiert. Dafür müssten Paketformat,
Konvertierung, Pufferspeicher und Formatabgleich erweitert werden. Bei gleicher Samplerate
und Kanalzahl steigt der reine PCM-Datenanteil um 50 %; Paket- und Netzwerkkosten kommen hinzu.
Samplerrate und Format müssen auf verbundenen Endpunkten kompatibel sein.

## „Stolpern“, Knacken oder Verzerrung

Abgehackter Ton ist kein typisches Zeichen zu geringer Bittiefe. Mögliche Ursachen sind:

- Fehlende/verspätete Pakete oder Unterlauf eines Audiopuffers.
- Aussetzer im Quellprogramm oder Audiotreiber, Scheduling und lokale Geräteprobleme.
- Clock-/Resampling-Probleme bei unabhängig laufenden Endpunkten.
- Clipping bei lauten Spitzen, das als hartes Knacken oder Verzerrung hörbar werden kann.

Die genaue Ursache der gemeldeten Störung ist noch nicht nachgewiesen. Zuerst eine feste
Testquelle bei unveränderten Einstellungen vergleichen und Zähler beobachten. Bei einer
Abhängigkeit von lauten Spitzen den Quellpegel **vor** der Bridge reduzieren. Bereits am
Mikrofoneingang oder in der Quellanwendung geclipptes Audio wird durch spätere Absenkung nicht repariert.

Mehr Bittiefe oder eine höhere Samplerate behebt Paketverlust und Buffer-Underruns nicht.
Die lokale Bridge-Driftkorrektur ist keine PTP-Synchronisation und garantiert keine gemeinsame
Phase mehrerer PCs. Für belastbare Audioqualität sind Hör-, Mess- und Langzeittests nötig.

## Weitere Dokumentation

[Übersicht](./uebersicht.md) · [Installation](./installation.md) · [Controller](./controller.md) ·
[Audio Bridge](./audio-bridge.md) · [Virtual Soundcard](./virtual-soundcard.md) ·
[Puffer und Latenz](./puffer-latenz.md) · [Recovery](./recovery.md) · [Diagnose](./diagnose.md)
