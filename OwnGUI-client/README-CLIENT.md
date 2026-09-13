# OwnGUI – neue Client-Funktionen

## Grafik

Im Menü **Grafik** kann zwischen **Automatisch** und **DirectX 11 erzwingen** gewählt werden. DirectX 11 wird über Unitys Startparameter `-force-d3d11` aktiviert. Eine Änderung startet das Spiel automatisch neu.

Die MovementMod-DLL enthält bereits ImGui-Backends und Render-Hooks für DirectX 9, 10, 11 und 12. Für die gewünschte Client-Auswahl sind daher keine weiteren DX11-Änderungen an der DLL nötig.

## HTTP(S)-Proxies

1. **Proxy > Proxy-Liste öffnen...** auswählen.
2. Je Zeile einen Proxy als `host:port:benutzer:passwort` eintragen.
3. **HTTP(S)-Proxy verwenden** aktivieren.

Die Reihenfolge ist fest und wird in `client.ini` gespeichert: erster Proxy, zweiter Proxy, alle weiteren Proxies der Reihe nach, anschließend eine Phase ohne Proxy und danach wieder der erste Proxy. Nach jeder zufälligen Laufzeit zwischen 6 und 8 Stunden wird das Spiel sauber geschlossen und mit der nächsten Stufe gestartet. Nur die Dauer ist zufällig; die Proxy-Reihenfolge ist es nicht. Das Intervall steht unter `MinHours` und `MaxHours`.

Wichtige technische Grenze: Der Client setzt `HTTP_PROXY` und `HTTPS_PROXY` für den Spielprozess. Das funktioniert nur, wenn die vom Spiel verwendete Netzwerkbibliothek diese Variablen auswertet. Direkte TCP/UDP-Verbindungen werden dadurch nicht transparent umgeleitet. Dafür wäre ein lokaler Tunnel oder eine gezielte Netzwerkanbindung in der DLL erforderlich.

`proxies.txt` enthält Zugangsdaten und darf nicht ins öffentliche GitHub-Repository eingecheckt werden.

## Prozess-Isolierung

Eine benannte Mutex-Sperre verhindert, dass der Client zweimal gleichzeitig gestartet wird. Der Spielprozess wird außerdem einem Windows-Job-Objekt mit `KILL_ON_JOB_CLOSE` zugeordnet. Dadurch bleiben beim Beenden oder Absturz des Wrappers keine verwaisten Spielprozesse zurück.

## Nur eine DLL

Der Launcher lädt nur noch `EveryDayiamNEYDISCH.dll`. Das aktuelle MovementMod-Projekt verweist für Release x64 bereits auf `C:\vcpkg\installed\x64-windows-static` und linkt `minhook.x64.lib`; MinHook ist damit statisch Bestandteil der Haupt-DLL. `minhook.x64d.dll` darf aus dem Release-Paket entfernt werden.

Die fertige DLL sollte als Release-Asset im Repository `NEYDISCH/movementmod-updates` veröffentlicht werden. Für einen späteren automatischen Download sollte zusätzlich eine SHA-256-Prüfsumme oder digitale Signatur geprüft werden.
