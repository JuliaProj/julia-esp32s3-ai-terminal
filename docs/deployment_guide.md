# Julia Voice Backend Deployment

## SD Layout

```text
/julia/
  audio/          Offline prompts and mock TTS, raw 16 kHz mono s16le PCM
  transitions/    Full-screen TRN transitions
  idle/           Optional idle assets
  memory/         Conversation and profile data
```

## Offline Audio

Generate and deploy resources:

```powershell
python tools/generate_offline_tts.py
python tools/deploy_offline_audio.py --port COM5
```

Validate one resource with `audio resource net_offline`. Use `audio stop` to interrupt it.

## Firmware

Build with ESP-IDF 5.5.4, then flash the application partition on COM5. Mock builds use:

```text
CONFIG_JULIA_MOCK_ASR_ONLY=y
CONFIG_JULIA_LLM_MOCK_MODE=y
```

Disable both options only after the microphone and cloud credentials are available.

## Console Commands

```text
asr "你好"
dialog status
dialog reset
audio resource <name>
audio play /sdcard/path.pcm
audio stop
audio rms
wifi status
wifi scan
wifi connect <ssid> <password>
mem psram
test leak start
test leak status
test leak stop
```

## Troubleshooting

- `ESP_ERR_NOT_FOUND`: verify the SD path and run the deployment script again.
- No cloud response: check `wifi status`, DNS logs, API URL, key, model, and system time.
- Audio is distorted: resources must be signed little-endian 16-bit, 16 kHz, mono PCM.
- USB reconnect resets the board: open USB Serial/JTAG with DTR and RTS deasserted.
- `ESP_ERR_NO_MEM`: run `mem psram`; transition files should remain demand-loaded.
