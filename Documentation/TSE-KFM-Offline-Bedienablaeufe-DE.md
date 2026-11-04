> Aktueller getesteter Stand: [28.10.2026, Version2026.10.28.6](TSE-KFM-Stand-20261028-DE.md). Der folgende Text dokumentiert eine frühere Phase.

# Vollständige Bedienabläufe: Offline-Prüfung für 2026.10.27.10

Stand 27.10.2026. Keine neue Hardwareverbindung, kein Flashen, keine Motorbewegung. Zuletzt am Gerät geprüft ist weiterhin Version .8. Die neue .10 ersetzt den vorbereiteten .9-Kandidaten für die nächste gemeinsame Runde. Die bisherigen Grenzen bleiben: explizite Freigabe, frischer Funkkontakt, höchstens 500 U/min und ein 30-Sekunden-Betriebsfenster.

## Aufbau und Grenzen der Prüfung

`tools/test_kfm_flows.py` entnimmt die aktuellen Start-/Stop-Funktionen aus `diag_console.cpp`, Purge-Funktionen aus `main.cpp` sowie Gewichtsablauf und Vorhersage aus `gbw.cpp`. Diese echten Funktionen werden zusammen mit dem echten KFM-Backend kompiliert. Der Test bildet den Zustandsaufruf deterministisch nach. Waage, USB/Funk-Transport, Menüwerte und mechanische Drehzahlreaktion sind Testmodelle; das Lernverfahren wird durch einen Platzhalter ersetzt. Der vollständige Arduino-Loop, echte Funkpakete, BLE-Latenzen und Hardwarewirkung sind damit nicht nachgewiesen. Separate UART-Tests prüfen weiterhin das echte Modbus-Protokoll.

Die elf Szenarien laufen jeweils normal und mit Address-/UndefinedBehavior-/Float-Cast-Sanitizern:

| Ablauf | Geprüftes Ergebnis |
| --- | --- |
| Manuelles Mahlen | Start ohne Freigabe abgewiesen, Start mit Freigabe, Drehzahländerung einschließlich Begrenzung, Stop und Null-Rücklesewerte |
| Manuelles Laufzeitende | Rückkehr in den Ruhezustand, Sollwert null und erneuter Start ohne Freigabe abgewiesen |
| Button-Purge | Übergang vom Mahlen zu Purge, beide Richtungen, Abschluss sowie vorzeitiger Stop |
| Funkverlust | Stop-Anforderung und verworfene Freigabe; Funkrückkehr erlaubt keinen selbsttätigen Neustart |
| Gewichts-Ziel erreicht | Echte Vorhersagefunktion im simulierten Gewichtsverlauf, Ende des Mahlvorgangs |
| Kein Mahlgut | Regulärer Abbruch mit Fehler 105 und Sollwert null |
| Waage verloren | Abbruch mit Fehler 104, Sollwert null und verworfene Freigabe |
| Ohne verbundene Waage | Fehler 106, kein nichtnulliger Mahlbefehl |
| RS485-Ausfall während GBW | Lokale Stop-Anforderung und Sperre; nach Rückkehr kein Neustart. Kein Nachweis eines am getrennten Treiber angekommenen Stops. |
| Messwertspeicher voll | Grenze bei 300 Einträgen und weitere 400 Anhänge ohne Speicherüberschreitung; zeitliche Reihenfolge bleibt erhalten |
| Schneller erneuter GBW-Start | Nach Stop-Prüfung und neuer Freigabe ein frischer Mahlvorgang, auch unterhalb der früheren Ein-Sekunden-Heuristik |

## Gefundene und behobene Fehler

1. Die Gewichtserfassung erhöhte den Index vor der Grenzprüfung und schrieb dadurch auf Eintrag 300 eines Felds mit 300 Elementen. Der Sanitizer bestätigte den Speicherüberlauf. Jetzt werden bei vollem Speicher die ältesten Werte verworfen und die restlichen zeitlich geordnet gehalten. Das Lernverfahren wird für einen solchen unvollständig gespeicherten Mahlvorgang ausgelassen.
2. Die Überbrückung einer Nullpause hielt die Prüffreigabe auch im GBW-Ruhezustand aufrecht. Der neue Ablauftest bestätigte das nach Waagenverlust. Pausen dürfen die Freigabe jetzt ausschließlich innerhalb eines aktiven Mahl-/Purge-Ablaufs behalten; ein beendeter Betrieb verwirft sie.
3. Für einen neuen GBW-Start wird jetzt ausdrücklich der Startzustand zurückgesetzt. Die frühere Zeit-Heuristik allein konnte bei schnellem erneutem Start den alten Mahlzustand weiterverwenden. API/Funk und lokale Taste rufen die neue Vorbereitung auf. Der schnelle Neustart ist im Ablauftest enthalten.

## Diagnoseauswertung ohne angeschlossene Hardware

`tools/kfm_session_report.py` liest ausschließlich vorhandene `events.jsonl`-Dateien. Beispiel vom Repository-Verzeichnis:

```sh
python3 tools/kfm_session_report.py outputs/kfm-touch-session-20261027-185209/controller-radio-off-round1 --output outputs/meine-neue-auswertung
```

Das Zielverzeichnis muss neu sein. Es entstehen `timeline.csv`, `analysis.json` und `REPORT_EN.md` für die Weitergabe. Enthalten sind Soll-/Ist-Drehzahl, Zustand, Fehler, gemeldete Gründe, Freigabe und verfügbare Rücklese-/Funkdaten. Ältere oder nicht verfügbare Felder bleiben leer. Insbesondere liefert .9/.10 derzeit kein eigenes Funkfrische-Feld im Kandidaten-Datensatz; ein nicht verfügbares Test-Funkfeld wird nicht als tatsächlicher Funkfehler dargestellt.

Gründe bleiben in zeitlicher Reihenfolge erhalten, einschließlich späterer Stop-/Aufräumbefehle. Es gibt keine automatische Hardware-Freigabe oder pauschale Bewertung „bestanden“. Der Bericht überträgt Software- und Registerbeobachtungen und ersetzt weder Beobachtung des Motors noch den Nachweis einer unabhängigen Abschaltung.

Eine Auswertung des vorhandenen echten Funk-Tests liegt unter `outputs/kfm-v10-offline-validation/radio-session-analysis/`: 89 Statuszeilen, gemeldete Spitze 100 U/min und keine gemeldeten Fehlercodes. Die ursprünglichen Rohdaten bleiben unverändert.

Als Nächstes folgt die gemeinsame [Funktionsprüfung](TSE-KFM-Funktionspruefung-DE.md). Kurze Purge-Vibrationsfenster, echte Waagen-/Funkreaktion, höhere Drehzahlen, Lastbetrieb und Ausfall einer unabhängigen Stop-Möglichkeit bleiben Hardware-Abnahmepunkte.
