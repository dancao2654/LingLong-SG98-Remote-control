# Begrenzter Touchscreen-Start/Stop-Test: Version 2026.10.27.8

Die vorherige .7-Prüfung erreichte tatsächlich100U/min über temporäres PA25, stoppte automatisch und wurde vom Bediener beobachtet. Freigabe, Presets und Ist-Drehzahl wurden anschließend unabhängig mit0 bestätigt. Version .8 ist inzwischen bei ausgeschalteter Mühle app-only installiert und per USB geprüft. Die Testfreigabe ist nicht gesetzt, der Puls ist idle, kein erfolgreicher Servo-Schreibbefehl wurde gesendet. Frische Funkdaten des eingeschalteten Touchscreens werden erkannt. Noch ist kein Touchscreen-Lauf erfolgt. Nachweise: `outputs/kfm-touch-test-device-20261027/`.

## Rahmen

Dies ist eine eigene **Prüfversion**, keine freigegebene Alltagssteuerung. Sie fährt unabhängig vom gespeicherten Sollwert1100 fest100U/min in der zuvor getesteten Draht-Richtung−100. Pro Freigabe ist nur ein Start erlaubt, mit höchstens5000ms angefordertem Nichtnull-Sollwert. Softwarezeiten sind keine unabhängige Sicherheitsabschaltung; CPU-, Controllerstrom- oder RS485-Ausfall können einen Stop verhindern. Der Bediener bleibt an der leeren geschlossenen Mühle direkt am Main Power Switch.

Die USB-Konsole kann den Test nur freigeben oder sperren, sie sendet selbst keinen Start-/Stop-Befehl für den Nachweis. Der normale `api tse arm` und der direkte USB-Pulsstart bleiben gesperrt. Erst eine frische ausdrückliche USB-Testfreigabe und danach der echte Startbefehl des Touchscreens können den bekannten PA25-Puls starten. Vorher gelten dieselben KFM-/Einschalt-/Stillstandsprüfungen wie in .7, einschließlich bestätigter Freigabe bei PA25zero vor dem Nichtnullwert. Öffentliche Rohschreibbefehle, andere Drehzahlen/Presets und persistente Schreibbefehle bleiben gesperrt.

Eine Freigabe verfällt nach30000ms, bei Moduswechsel oder fehlendem frischem ESP-NOW-Verkehr. Während eines Laufs fordert ausbleibender ESP-NOW-Befehlsverkehr über3000ms den Stop an. Nach Stop, Zeitablauf oder Funkverlust wird nicht automatisch wieder freigegeben. Rückkehrender Funk startet nichts. Diese Fälle sind zunächst offline getestet; ihre realen Fehlerpfade sind noch zu prüfen.

Die bestehende Start-/Stop-Kommandostrecke bleibt die des Touchscreens. Stop wird in diesem Test unmittelbar in die bekannte Null-/Freigabe-Rücksetzung übersetzt, ohne Auto-Purge. GBW, Purge, veränderliche Drehzahlen und andere Mahlabläufe sind in dieser Stufe nicht der Testgegenstand; gespeicherte Einstellungen werden dafür nicht umgeschrieben. Ein erfolgreiches Start-/Stop-Paar ist noch kein Nachweis für deren spätere Funktion.

## Gemeinsamer Ablauf

1. Main Power Switch aus. Gerät/MAC/App-Offset prüfen, nur das geprüfte .8-App-Image installieren. Start und Sperren prüfen, ohne Freigabe oder Motorstart.
2. Main Power Switch auf Ansage ein. Frische Einschaltwerte, KFM/Kommunikation, Fehler0, Drehzahl0 und Freigabe aus prüfen. Der Touchscreen muss separat eingeschaltet sein und frische ESP-NOW-Befehle liefern. Noch keinen Start drücken.
3. Bediener bestätigt Bereitschaft am Hauptschalter und Zugriff auf die Start-/Stop-Tasten. Danach wird erst die einmalige USB-Freigabe gegeben. Sie aktiviert allein noch keinen Motor.
4. Auf Ansage am Touchscreen **Start**, etwa zwei Sekunden warten, dann **Stop** drücken; keine Zwischenantwort nötig. Nicht am Drehzahlsteller drehen. Der tatsächliche Testwert ist100, auch wenn der gespeicherte Sollwert1100 angezeigt bleibt. Bei unerwartetem Verhalten sofort Main Power Switch aus.
5. Erst danach berichten, ob die Mühle gestartet und nach der Stop-Taste gestoppt hat. Die maximal fünf Sekunden sind nur eine zusätzliche Softwarebegrenzung. Falls der Lauf selbst am Fünf-Sekunden-Limit endet, belegt dies nicht die Stop-Taste; die Ursache wird anhand der Rohlogs getrennt bewertet.
6. Freigabe, PA25, Auswahl und Ist-Drehzahl unabhängig lesen, Logs sichern, Main Power Switch aus. Weitere Starts brauchen eine neue ausdrückliche Freigabe.

## Werkzeuge

```sh
python3 tools/kfm_touch_test_operator.py --port /dev/cu.usbmodemXXXX --expected-mac B8:1F:3F:C8:F5:30 --operator-present
```

Die Befehle sind `arm`, `disarm`, `restore`, `quit`; `off`/`on` protokollieren nur menschliche Schalterbetätigungen. Die Konsole enthält keinen Start-/Stop- oder Pulsbefehl. Die Freigabe prüft frische Identität, passenden .8-Build, vorhandene Funkdaten und stationären fehlerfreien Grundzustand. Bei Beendigung wird gesperrt und, falls ein Lauf beansprucht wurde, der Rücklesestatus verlangt. Eine Freigabe ohne jede Motoroperation wird gesondert als unbetätigt gesperrt protokolliert.

`restore` darf nach erneuter Identitäts-/Konfigurationsprüfung nur bekannte Null-/Stopwerte senden. Eine erfolgreiche Schreibbestätigung allein ist kein Nachweis des Stillstands. Bei fehlender Null-Rücklesung Main Power Switch aus lassen.

## Offen vor einer Alltagsfreigabe

Der reale Touchscreen-Test, Funkverlust-Stop und gegebenenfalls automatischer Zeitstop sind noch zu beobachten. Kurze Netzunterbrechungen, RS485-Trennung, Verlust der Controller-Stromversorgung, Last und höhere/veränderliche Drehzahlen bleiben zusätzliche offene Fälle. Firmware kann einem nicht mehr erreichbaren Treiber keinen Stop zustellen. Eine wirklich wirksame treiberseitige Kommunikationsüberwachung oder unabhängige Freigabe-/Abschaltkette muss für solche Ausfälle geklärt werden; aus der beschriebenen allgemeinen Kommunikationsfehler-Option allein wird kein nachgewiesener Bus-Stille-Watchdog abgeleitet. Diese Prüfversion wird deshalb nicht als unbeaufsichtigter normaler Betrieb freigegeben.

Präzisierung zur Speicherung: Gesperrt sind explizite persistente PA-Parameterschreibbefehle. Die bekannten SON-/SP1-Steuerregister werden für die Bedienung weiterhin beschrieben; ihre mögliche dauerhafte Wirkung ist nicht ausgeschlossen. Eine frühere pauschale Formulierung „keine EEPROM-Schreibvorgänge“ darf dafür nicht als Nachweis gelesen werden.
