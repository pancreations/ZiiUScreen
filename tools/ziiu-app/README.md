# ZiiUScreen desktop app

A Windows front end for the existing stream scripts: start, stop and reset the
stream, change its settings, see live status, use the controller tools and read the logs.

```powershell
powershell -ExecutionPolicy Bypass -File tools\ziiu-app\build.ps1
build\ziiu-app\ZiiUScreen.exe            # UAC prompt: the radio needs admin
build\ziiu-app\ZiiUScreen.exe --tray     # start hidden in the notification area
```

| Action | What runs |
|---|---|
| Start | `tools\ziiu-drc\run.ps1` with the saved settings, hidden; output in `.local\stream\launcher.log` |
| Stop | `ziiuctl stream-stop`; run.ps1's own cleanup stops the encoder and controller |
| Reset | Stop, wait for the launcher mutex (as `reset.ps1` does), Start |
| Calibrate / rumble / sensor bar | UDP to the running stream (50222, 50200), as `calibrate.ps1`, `rumble.ps1`, `ir.ps1` |
| Wi-Fi card | `tools\wifi-mode.ps1 Status / Normal / GamePad` |

Settings live in `.local\app\settings.json`. They default to run.ps1's defaults and
apply on the next start or reset. The dashboard reads `.local\stream\*.log` and
`.local\gamepad-link\hostapd.log`; it never talks to the GamePad directly.

Layout: `Backend\` holds the logic and has no UI code (paths, settings, stream
control, log parsing, display and audio lists). `MainWindow` holds the pages and
`Tray` the notification-area icon.
