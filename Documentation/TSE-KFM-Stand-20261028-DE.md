# Getesteter KFM-Stand vom 28.10.2026

Installiert ist **2026.10.28.6**, Build `controller-kfm-run-candidate`, auf dem Waveshare ESP32-S3-RS485-CAN (Controller-MAC `B8:1F:3F:C8:F5:30`). Der Touchscreen wurde heute nicht neu geflasht. Diese Datei ersetzt für den aktuellen Stand die älteren Prüfpläne; deren Versionsangaben bleiben historische Nachweise.

Der letzte echte Mahltest erreichte bei **18,0 g Soll exakt 18,0 g laut Bookoo und Controller**. Hauptphase800 U/min, Schlussphase300 U/min; höchste abgetastete Ist-Drehzahl802 U/min. Zwischen erstem abgetastetem Laufbefehl und Nullbefehl lagen27,80s. Danach Fehler0, Ist-Drehzahl0, automatische Sperre der Motorfreigabe und vollständiger Stop-Lesenachweis (Maske63). Das ist ein einzelner erfolgreicher Bezug, noch keine nachgewiesene Wiederholgenauigkeit.

## Gesicherte Einstellungen

| Einstellung | Wert |
|---|---:|
| Zielgewicht | 18 g |
| Hauptphase / Schlussphase | 800 / 300 U/min |
| Wechsel zur Schlussphase | 4,5 g vor Ziel |
| Startverzögerung | 100 ms |
| Geschwindigkeitsfaktor | 2100, aus dem erfolgreichen echten Mahltest gelernt |
| Zeitkorrektur | 150 ms |
| Button-Purge | aus |

Die ursprünglichen Werte sind zusätzlich gesichert; der Geschwindigkeitsfaktor war2500. Beim Abschluss wird ausschließlich der nachvollziehbar gelernte Wert2100 erhalten. Der finale USB-Lesenachweis befindet sich im Übergabepaket unter `evidence/gbw-tests-round6/final-preserved-settings.json`.

## Physische Nachweise mit Versionszuordnung

| Prüfung | Version | Ergebnis |
|---|---|---|
| Touchscreen Start/Stop100,200,500 U/min | 2026.10.28.1 | bestanden, leere Mühle |
| Drehzahlwechsel200→500→200 | .1 | bestanden |
| Automatischer30-s-Stop | .1 | bestanden |
| Touchscreen stromlos während Lauf, Wiederkehr ohne Neustart | .1 | bestanden |
| Zeitgesteuerter Vor-/Rückwärts-Purge100 U/min | .1 | bestanden; ursprüngliche Menüs wiederhergestellt |
| Bookoo verbinden, trennen, wiederverbinden; Becher abnehmen/aufsetzen | .4 |0/68,1 g korrekt übertragen |
| Automatisch tarieren und Stop bei fehlendem Auswurf | .4 | bestanden; erwartete Meldung105 |
| Reale BLE-Trennung am Controller während Lauf | .5 | bestanden; Meldung104, Stillstand, Wiederverbindung ohne Neustart |
| Externes46,1-g-Silikonteil überschreitet18-g-Ziel | .5 | Abschaltung bestanden; kein Genauigkeitstest |
| Erster beladener Versuch200 U/min | .5 | nur~0,2 g; Stop105 statt Zielabschluss |
| Echter Mahltest800/300 U/min auf18 g | .6 |18,0 g und fehlerfreier automatischer Stop |

Der erste physische Ausschaltversuch der Bookoo ist ausdrücklich **nicht eindeutig**: Druck auf die Wiegefläche erzeugte~125 g und löste den Gewichtsstopp vor dem Verbindungsabbruch aus. Die spätere BLE-Fehlernachstellung prüft die Software-Reaktion auf eine tatsächliche Trennung am Controllerende, nicht auf einen beliebigen Funkfehler. Fehlstarts nach abgelaufener30-s-Freigabe sowie ein zu spät aufgelegtes Silikonteil sind separat dokumentiert.

## Aktive Grenzen der installierten Testversion

Nach Boot und nach jedem Lauf ist die Motorfreigabe gesperrt. **Jeder Lauf braucht weiterhin eine ausdrückliche USB-Sitzungsfreigabe**; der Touchscreen allein reicht im gesperrten Zustand nicht. Freigabe ohne Start verfällt nach30s, ein Lauf ist ab erstem Nichtnull-Sollwert auf30s begrenzt. Zugelassene Drehzahl maximal800 U/min, Überdrehzahlschutz950 U/min, frische Fernbedienungsbefehle innerhalb3s erforderlich. Die Standardkonfiguration ohne expliziten800er-Buildschalter bleibt bei500 U/min.

Es werden temporärer PA25 und die gemessene SON/SP1-Belegung verwendet. PA24 wird in diesem Kandidaten nicht beschrieben. Ein früher bestätigter PA24-Nullschreibversuch überstand den beobachteten Kaltstart nicht; daraus folgt keine Aussage über sämtliche Speicherbereiche. Unkalibriertes drehmomentabhängiges Purge/Lernen bleibt für KFM ausgeschlossen. Die Software kann bei Ausfall des Controllers oder unterbrochener RS485-Leitung keinen Stop mehr senden; eine unabhängig wirksame Abschaltung wurde nicht nachgewiesen.

## Änderungen und Entwicklung

Die KFM-Schreibzustandsfolge verwirft ihre abgeschlossene Null-Freigabeprüfung nicht mehr bei einem durch den60-ms-Sendeabstand verzögerten Schreibversuch. Der Bookoo-Wiederverbindungsweg verbindet einen wiederverwendeten NimBLE-Client nur einmal und beseitigt ungültige Client-Zeiger bei Fehlern. Gewichtsmodus bleibt beim Abbruch erhalten. GBW-Verlauf ist begrenzt; Tarierung und schneller Neustart setzen das Vorhersagefenster korrekt zurück.

Der USB-Befehl `api tse scale_drop_test` steht nur im Kandidaten zur Verfügung und trennt ausschließlich bei bereits freigegebenem Gewichtslauf mit gemessener Bewegung die Waagenverbindung für5s. Er sendet keinen Motorstart. Er ist ein Diagnosebefehl und gehört nicht in den normalen Bedienablauf.

Offline prüfen und bauen:

```sh
python3 -m pip install -r tools/requirements-kfm-build.txt
python3 tools/run_kfm_checks.py --report outputs/kfm-checks.json
pio run -e controller-kfm-run-candidate -e controller-universal-s3 -j 2
```

Die Prüfungen öffnen kein Gerät. Sie enthalten echte Backend-/Transport-/Bedien-/GBW-Funktionen mit modelliertem UART, BLE oder Zeitablauf, einschließlich Sanitizern. Sie ersetzen keine realen Messungen. Beide SDK-Builds und die vollständige letzte Offline-Regression sind bestanden. Das gesicherte App-Image wird nur an0x10000 mit passender bestehender Partitionierung eingesetzt; kein Full-Erase und kein Touchscreen-Image.

## Offen für später

Auf Wunsch des Nutzers gibt es jetzt keine weiteren Motorversuche. Höhere Drehzahlen in **beiden** Phasen sind als Wunsch festgehalten; konkrete neue Werte werden erst für eine weitere Prüfrunde festgelegt. Vor einer Alltagsversion stehen Wiederholbezüge, Drehzahl-/Vorhersageabstimmung sowie eine separat geprüfte Freigabestrategie ohne USB aus. Ein Entfernen der Freigabe- oder Zeitgrenzen ist nicht Bestandteil dieses abgeschlossenen Teststands. Die heute geladenen Bohnen können teilweise noch in der Mühle sein; beim nächsten Start diesen Zustand berücksichtigen.
