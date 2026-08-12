# Julia Avatar Serial Commands

The maintenance console runs on COM5 at 115200 baud.

| Command | Behavior |
| --- | --- |
| `state <0-19>` | Select a product sub-state and interrupt any active blink/transition. |
| `speak <rms>` | Set the mouth directly: 0-15 idle, 16-50 speak1, 51-80 speak2, 81+ speak3. |
| `mouth <0-3>` | Legacy 1.5-second mouth shape test. |
| `button down` | Simulate button press. After 600 ms, random RMS 0/30/80 is applied every 100 ms. |
| `button up` | End simulated long press and close the mouth. |
| `demo on` | Start automatic state and mouth cycling immediately. |
| `demo off` | Stop automatic cycling and close the mouth. |
| `demo status` | Print whether Demo is currently active. |
| `status` | Print uptime, heap, minimum heap, PSRAM, RSSI, and reset reason. |

Without console or button activity, Demo starts automatically after 30 seconds.
The repository has no physical button GPIO driver, so board firmware should call
`avatar_face_button_set_pressed(true/false)` from its button callback when that
driver is added.
