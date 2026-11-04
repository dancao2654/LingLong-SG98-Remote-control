# Erster begrenzter PA25-Lauftest: Version 2026.10.27.7

Version .7 ist app-only installiert; Start, Gerätekennung und Schreibsperren wurden bei ausgeschaltetem Servo per USB geprüft. Der Puls ist verfügbar, aber idle; es wurde kein Puls angefordert und kein erfolgreicher Servo-Schreibbefehl gesendet. Ein echter Motorlauf steht noch aus. Nachweise: `outputs/kfm-pa25-pulse-device-20261027/`. Die vorherige .6-Prüfung bestätigte bei deaktivierter Freigabe den Rückfall des temporären PA25-Prüfwerts 1 auf 0 nach der vom Bediener gemeldeten 60-Sekunden-Auszeit. Das belegt noch keine sichere Wiederanlaufsperre unter allen Bedingungen.

## Fester Prüfumfang

Das eigene Image `controller-kfm-pa25-pulse` lässt normale Starts am Touchscreen und die allgemeine USB-Freigabe weiterhin gesperrt. Nur der besondere Befehl `api tse pa25_pulse start` beantragt einen Puls. Er verwendet fest **100 U/min** und eine vorgesehene Laufzeit von **2000 ms**. Der im normalen Menü gespeicherte Sollwert 1100 wird dafür nicht verwendet. Der feste Drahtwert ist −100, entsprechend der aus den Originalcontroller-Aufzeichnungen übernommenen normalen Mahlrichtung. Die tatsächliche Drehrichtung muss beim Test beobachtet werden.

Vorbedingung ist eine gültige unveränderte Einschaltaufzeichnung mit PA25=0, Freigabe aus, Auswahl2 und Ist-Drehzahl0. Danach liest die Firmware erneut den bekannten KFM-Grundzustand: Geschwindigkeitsquelle/-modus, Laufzeit-Presets PA24/PA25=0, SON=0, PA53=0, physische und virtuelle Eingangsmeldungen0, bekannte Eingangsbelegung, Auswahl2, alarmfreier Stillstand. Erst dann fordert sie SON=1 bei PA25=0 an. Eine vollständige Rücklesung muss die Freigabe bei weiterem Stillstand und Preset0 bestätigen. Erst danach schreibt sie temporär PA25=−100.

Nur das private Puls-Schreibfenster kann SON=1 und diesen einen Drehzahlwert senden. Öffentliche Rohschreibbefehle, variable Drehzahlen, PA24-Laufwerte, andere Presets und persistente Parameter bleiben gesperrt. Die Pulsvariante kann nicht zusammen mit den vorherigen Prüfvarianten oder normaler Motorfreigabe gebaut werden.

Während des Pulses werden Ist-Drehzahl, Alarm und Konfiguration wieder gelesen. Beobachtete Drehzahlbeträge über150U/min oder eine unerwartet positive Drehzahl über5U/min führen zum Stop-Versuch. Neue Nichtnull-Sollwerte werden nicht nachgesendet. Fehlende/ungültige Antworten, geänderte Eingänge, Alarm, Abbruch oder geänderter Bedienzustand brechen den Lauf ab. Das bisher ungeprüfte Roh-Drehmoment wird nicht als kalibrierte Prozentgrenze verwendet; Werkseinstellungen für Strom, Drehmoment und Beschleunigung bleiben unverändert.

## Stop und Grenzen

Nach der vorgesehenen Laufzeit werden alle vier Befehle versucht: SON=0, temporär PA25=0, temporär PA24=0, Auswahl2. Auch nach einer verlorenen Bestätigung werden die übrigen Stop-Befehle versucht. Schreibantworten und tatsächliches Rücklesen bleiben getrennte Kriterien. Bei ansonsten passender Konfiguration und begrenztem Nachlaufen folgt eine reine Lesephase von höchstens weiteren3000ms. Erst eine gültig gemeldete Drehzahl0 kann einen erfolgreichen Stop ergeben. Ein Alarm, ungültige Konfiguration oder fehlende Rücklesung zählt nicht als Erfolg. Es gibt keinen automatischen zweiten Puls.

**Die Zeit- und Drehzahlgrenzen sind Softwarebedingungen, keine unabhängige Sicherheitsabschaltung.** Eine blockierte CPU, ausgefallene Versorgung oder gestörte Kommunikation kann den Stop verhindern oder verzögern. Die zwei Sekunden garantieren keine physikalisch exakt zweisekündige Bewegung; UART-Arbeit und Nachlauf sind zu berücksichtigen. Der Bediener muss bei der leeren geschlossenen Mühle direkt am Main Power Switch bleiben und bei unerwartetem Verhalten sofort ausschalten. Eine erfolgreiche erste Runde ist noch keine Freigabe für unbeaufsichtigten Betrieb, Last, beliebige Drehzahlen oder kurze Netzunterbrechungen.

## Ablauf mit Bediener

1. Main Power Switch aus. Geprüftes App-Image .7 nach bestätigter Gerätekennung/Adresse app-only installieren; kein Löschen der NVS oder Partitionen. Start und Schreibsperren zunächst bei ausgeschaltetem Servo prüfen. Noch keinen Puls anfordern.
2. Main Power Switch auf Ansage ein. Frische Einschaltaufzeichnung, KFM-Erkennung, Fehler0 und Stillstand prüfen. Bei unerwartetem Zustand nicht starten. Der gespeicherte Sollwert1100 ist kein Prüfbefehl.
3. Vor dem ersten tatsächlichen Lauf ausdrücklich bestätigen: Mühle leer und geschlossen; Hände/Werkzeuge aus dem Arbeitsbereich; Bediener am Hauptschalter; bereit für den einzelnen100-U/min-Puls. Kein Start am Touchscreen.
4. Die besondere Konsole startet nur die Protokollierung. Erst das ausdrücklich eingegebene Wort `pulse` beantragt den Lauf. Mögliche kurze Bewegung und Richtung beobachten; bei Bedarf sofort Hauptschalter aus. Währenddessen nichts am Touchscreen drücken.
5. Automatischen Stop abwarten. Gemeldete Spitzen-Drehzahl, Bewegung, Alarm, Stop-Antworten und Rücklesung prüfen. `pulse_completed_no_motion_observed` ist kein Nachweis einer tatsächlich erfolgten Bewegung. Drehzahl- und Zeitbegrenzung sind keine mechanische Sicherheitsgarantie.
6. Danach SON/PA25/Auswahl und Ist-Drehzahl unabhängig direkt lesen; den Bediener nach tatsächlicher Bewegung, Richtung und Stillstand fragen. Anschließend Main Power Switch aus. Erst anhand des Ergebnisses über die nächste Änderung entscheiden.

## Werkzeuge und Wiederherstellung

```sh
python3 tools/kfm_pa25_pulse_operator.py --port /dev/cu.usbmodemXXXX --expected-mac B8:1F:3F:C8:F5:30 --operator-present
```

`pulse`, `cancel`, `restore`, `quit` sind die Bedienbefehle; `off`/`on` protokollieren nur menschliche Schalterbetätigungen. Es wird kein Schalter ferngesteuert. `cancel` versucht den Stop. Nach verlorener Sitzung kann `restore` nach erneutem KFM-/Konfigurationsnachweis nur die bekannten Null-/Stopwerte senden. Dafür werden nur PA25=0 oder der bekannte feste Drahtwert−100 sowie die bekannte Auswahl akzeptiert. Ein Alarm wird nicht gelöscht. Die Operator-Konsole verlangt beim Beenden nach einer beanspruchten Operation zusätzlich zur Abbruchantwort einen passenden Rücklesestatus. Bei unbestätigtem Stop Main Power Switch aus lassen.

Normale Motorstarts bleiben auch nach einem erfolgreichen Puls gesperrt. Eine vollständig bedienbare PA25-Betriebssteuerung, Kommunikationsausfall-Verhalten, Controller-Neustart, kurze Netzunterbrechungen und Lasttests sind weitere getrennte Schritte.
