# PA25-Prüfung: Version 2026.10.27.6

Version .6 ist inzwischen app-only auf dem isolierten ESP32 installiert. Start, Version, Schreibsperren und Verweigerung der Rücksetzung ohne angeschlossenen Servo wurden per USB geprüft. Der Prüfwert wurde nicht gesetzt; die eigentliche PA25-Prüfung steht noch aus. Nachweise: `outputs/kfm-pa25-device-v6-20261027/`. Diese Version ist eine eigene Diagnose-Firmware für den Waveshare-Controller B8:1F:3F:C8:F5:30. Sie ist keine freigegebene Betriebsfirmware. Die vorherige Version .5 ist app-only auf dem allein per USB angeschlossenen Controller installiert. Gerätekennung, Start und die lokalen Sperren gegen Freigabe, öffentlichen PA25-Prüfwert und persistente Schreibbefehle wurden geprüft. Der Sollwert 1100 blieb erhalten. Der PA25-Test ist weiterhin ungestartet; es gab keine erfolgreichen Servo-Schreibbefehle bei dieser isolierten Prüfung. Die eigentliche Prüfung mit angeschlossenem Servo steht noch aus. Nachweise: `outputs/kfm-pa25-device-20261027/`.

## Zweck und Grenzen

PA25 wurde bisher beim Einschalten mit 0 gelesen; das ausgewählte Preset ist SP1=ON/SP2=OFF (erzwungene Auswahl 2). PA24 kehrte nach Unterbrechung zu −1000 zurück. In der bedienten .4-Prüfung blieb SON=1 nach der etwa 14 Sekunden beobachteten Kommunikationslücke erhalten. Daraus folgt noch kein vollständiger interner Netzreset. Bildschirm-Sollwertübertragung, Speicherung von 1100 U/min und automatische Funk-Wiederverbindung wurden separat mit ausgeschalteter Mühle bestätigt.

Die neue Prüfung setzt vorübergehend ausschließlich PA25 auf **1 U/min** über dessen temporäre Adresse 0x0099. Davor müssen SON=0, PA53=0, physische Eingangsmeldung 0, virtuelle Eingänge 0, die bekannte Werksbelegung, Auswahl 2, alarmfreie Ist-Drehzahl 0 und beide Laufzeit-Presets PA24/PA25=0 gelesen werden. Die Firmware sendet niemals SON=1, wählt kein anderes Preset und schreibt keine persistenten Parameter. PA26/PA27 sowie Drehmomentparameter bleiben unverändert. Die Eingangsmeldung 0 und Softwareprüfungen sind keine zertifizierte Abschaltung; ein Fehler oder eine externe Änderung kann Softwareannahmen ungültig machen. Deshalb bleibt der Test bedient, mit leerer geschlossener Mühle und erreichbarem Hauptschalter.

Ziel ist eine Aufzeichnung **vor** Rücksetzschreibbefehlen: Kommt PA25 als 0 zurück oder bleibt der Prüfwert 1 bestehen? Bleiben Auswahl 2, SON=0 und Ist-Drehzahl 0 erhalten? Mindestens drei vollständige Rückkehr-Abfragen über mindestens zwei Sekunden müssen konsistent sein. Ein Erfolg ist Evidenz für diese Prüfung; er beweist weder vollständigen Neustart noch sichere Wiederanlaufsperre in allen Fehlerfällen. Einen Motorlauf oder Betrieb mit 1100 U/min erlaubt dieses Image nicht.

## Ablauf der nächsten gemeinsamen Runde

1. Main Power Switch bleibt aus. ESP32 allein über seinen normalen USB-Port anschließen. Aktuellen Port, MAC und App-Offset prüfen. Nur das App-Image `controller-kfm-pa25-probe-2026.10.27.6.app.bin` an 0x10000 schreiben. Kein Factory-Image und kein pauschales Löschen der Einstellungen. Der Touchscreen wird nicht geflasht.
2. Ohne Servo Start/USB-Diagnose prüfen. `pa25_probe` muss verfügbar, `zero_probe` nicht verfügbar und die neue Prüfung in `idle` sein. Öffentliche SON=1-, PA25=1- und persistente Schreibversuche müssen lokal gesperrt sein. Keinen Probe-Start ausführen, solange der Bediener nicht bereit ist.
3. Erst auf Ansage Main Power Switch ein. Eine frische Aufzeichnung vor Backend-Schreibbefehlen, KFM-Erkennung, Initialisierung, Fehler 0 und Stillstand prüfen. Danach SON/Presets und Eingangsmeldung ausschließlich lesen. Bei abweichender Konfiguration keinen Marker setzen.
4. Die besondere Konsole mit explizitem Port, MAC und `--operator-present` starten. Sie protokolliert zunächst nur. Erst `probe` beantragt die beschriebene Firmware-Prüfung. Auf `waiting_for_power_off` und `holdVerified=true` warten; damit sind der Prüfwert 1 und die abgefragte deaktivierte Freigabe bestätigt. Bei Bewegung sofort Main Power Switch aus.
5. Auf Ansage ausschalten und `off` protokollieren. **Mindestens 60 Sekunden ausgeschaltet lassen**, während der ESP32 weiter über USB läuft und nur gelesen wird. Die Firmware hat insgesamt 180 Sekunden seit Prüfungsbeginn; bei Fristablauf wird abgebrochen und die Rücksetzung versucht. Nicht wegen einer abgelaufenen Frist hastig wieder einschalten. Eine längere Kommunikationslücke belegt weiterhin keine vollständige Entladung sämtlicher Versorgungskreise.
6. Erst auf Ansage wieder einschalten und `on` protokollieren. Erste und stabile Rückkehrwerte auswerten. Bei unerwarteter Freigabe, Bewegung, Alarm oder geändertem Preset wird die Rücksetzung versucht und die Prüfung als fehlgeschlagen behandelt.
7. Die Rücksetzung versucht alle vier Befehle: SON=0, temporär PA25=0, temporär PA24=0, Auswahl=2. Fehlende Bestätigungen oder fehlendes Rücklesen des Nullzustands dürfen nicht als Erfolg gelten. `restoreAcknowledged=true` und `restoreReadbackValid=true` müssen getrennt vorliegen. `cancel` kann eine unbestätigte Rücksetzung erneut versuchen. Danach separat SON/PA25/Auswahl und Ist-Drehzahl direkt auslesen. Anschließend Main Power Switch aus.

## Konsole und Offline-Prüfung

```sh
python3 tools/kfm_pa25_probe_operator.py --port /dev/cu.usbmodemXXXX --expected-mac B8:1F:3F:C8:F5:30 --operator-present
python3 tools/run_kfm_checks.py --report outputs/kfm-v5-checks.json
pio run -e controller-kfm-pa25-probe -j 2
```

Die Operator-Konsole akzeptiert nur `probe`, `cancel`, `restore`, `off`, `on`, `quit`. `off`/`on` protokollieren die tatsächliche menschliche Schalterbetätigung und schalten selbst nichts. Falsche oder veraltete Firmware, falsche MAC, alte Statuswerte oder ein nicht passender Grundzustand sperren den Start. Die Konsole ist vom früheren SON-Null-Preset-Werkzeug getrennt.

## Rücksetzung nach unterbrochener Sitzung oder ESP32-Neustart

Ein ESP32-Neustart löscht den lokalen Sitzungszustand. Ein im weiterhin versorgten Servo verbliebener PA25-Prüfwert 1 kann dann die normale Initialisierung blockieren. `cancel` allein bedeutet bei einer leeren Sitzung keinen Nachweis, dass dieser fremd gewordene Wert gelöscht ist.

Version .6 ergänzt deshalb den ausdrücklich eingegebenen Befehl `restore`. Er setzt keinen Prüfwert und sendet keine Freigabe. Die Firmware muss zuvor erneut den genauen KFM-Fingerabdruck, die Kommunikationsparameter und die bekannte Eingangskonfiguration lesen. Nur PA25=0 oder 1 sowie Auswahl2 sind für diesen besonderen Rücksetzpfad zulässig. Nach SON=0, PA25-temporär=0, PA24-temporär=0 und Auswahl2 werden die Werte tatsächlich wieder gelesen. Bestätigungen und gültiger Nullzustand einschließlich Drehzahl/Alarm sind getrennt sichtbar. Ein Alarm wird nicht gelöscht. Bei fehlendem Rücklesen bleiben weitere Prüfstarts gesperrt; eine erneute Rücksetzung ist möglich. Während einer aktiven Prüfung wird `restore` abgewiesen; dafür ist zuerst `cancel` vorgesehen.

Auch das Beenden der Operator-Konsole verlangt jetzt nach einer beanspruchten Prüf-/Rücksetzoperation eine Abbruchbestätigung und einen passenden Rücklesestatus. Andernfalls wird die unbestätigte Rücksetzung ausdrücklich protokolliert und Main Power Switch aus verlangt. Bei einer ersten Aufzeichnung nach einer solchen Rücksetzung wird korrekt markiert, dass bereits Backend-Schreibbefehle stattgefunden haben; sie darf nicht mehr als unveränderter Einschaltzustand gelten.

## Entscheidung danach

- **PA25 konsistent 0, SON=0, Auswahl 2:** Kandidat für eine spätere Anpassung der Drehzahlsteuerung an PA25. Noch kein automatisches Entsperren: Stop, Kommunikationsausfall, Controller-Neustart, Auswahlverhalten und echte Motorreaktion bleiben separat zu prüfen.
- **PA25 bleibt 1 oder ändert sich beim Booten:** Nicht auf Rückfall zu 0 vertrauen. Normale Motorstarts bleiben gesperrt; dokumentierte dauerhafte Einschaltkonfiguration oder unabhängige Wiederanlaufsperre weiter klären.
- **Rücksetzung unbestätigt:** Main Power Switch aus lassen. Zuerst den Zustand wieder lesbar und die Rücksetzung bestätigt machen; keine weitere Prüfung starten.
