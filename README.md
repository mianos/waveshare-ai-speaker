# ws-voice

Voice control bridge for a **Waveshare ESP32-S3 audio board**. On the wakeword
**"Hi ESP"** (detected on-device by esp-sr / WakeNet9) it records the following
speech, POSTs it to a configurable Whisper-style speech-to-text server, and
publishes the transcript to MQTT for Node-RED (or anything else) to act on.

```
mic (ES7210) ──▶ esp-sr AFE ──▶ WakeNet "Hi ESP" ──▶ capture (VAD-ended)
                                                          │
                                    WAV + multipart POST ─┘
                                                          ▼
                                   STT server ──▶ tele/<name>/stt {"text": …}
```

WakeNet is used **only as a trigger** — there is no on-device command
recognition. Meaning comes from the STT transcript, so the vocabulary is
whatever your STT model understands, not a fixed command list.

Part of the mianos ESP family; shares the `wifimanager`, `mqttwrapper`,
`settingsbase`, `webserver` and `jsonwrapper` components with `doorbell3`,
`ldr3` and `atomecho` (pulled from `github.com/mianos/mianesp` at build time).

## Hardware

- **ESP32-S3**, **16 MB flash**, **Octal PSRAM @80 MHz** (required — esp-sr's AFE
  and the capture buffer live in PSRAM).
- **ES7210** 4-mic ADC on I²C (SDA 11 / SCL 10) + I²S_NUM_1 (MCLK 12, BCLK 13,
  WS 14, DIN 15). **ES8311** speaker DAC on the same shared I²S bus (DOUT 16),
  used for tone cues and TTS playback. The SD card on the board is unused.
- **TCA9555** I/O expander gates board power/PA (including the speaker amp).
- **WS2812 RGB LED strip** (GPIO 38) — lights green while a wakeword is being
  captured.

> ⚠️ If the board turns out to have **Quad** (not Octal) PSRAM, boot will fail to
> map SPIRAM. Swap `CONFIG_SPIRAM_MODE_OCT` for `CONFIG_SPIRAM_MODE_QUAD` in
> `sdkconfig.defaults`. Confirm the module type before first flash.

## Build & flash

Requires ESP-IDF **v6.0.x**. `main/idf_component.yml` fetches the mianos
components over SSH (`git@github.com:mianos/mianesp.git`), so an SSH key with
access to that repo must be available at build time.

```sh
./build.sh                          # sets esp32s3 target on first run, then builds
idf.py -b 115200 flash monitor      # 115200 is the safe rate; try 460800 if stable
```

The first build also generates the esp-sr model image (`srmodels.bin`) and flashes
it into the `model` SPIFFS partition (see `partitions.csv`).

## Configuration

Settings persist in NVS as a single JSON blob (mianos `settingsbase`). Defaults
are compiled in (`main/Settings.h`); change them at runtime over MQTT or HTTP:

```sh
# MQTT
mosquitto_pub -t 'cmnd/wsvoice/settings' -m '{"stt_url":"http://host:8000/v1/audio/transcriptions"}'
# HTTP
curl -X POST -d '{"stt_url":"http://host:8000/v1/audio/transcriptions"}' http://<ip>/config
curl http://<ip>/config            # read current settings
```

| key            | default                     | meaning                                        |
|----------------|-----------------------------|------------------------------------------------|
| `stt_url`      | *(empty ⇒ STT disabled)*    | OpenAI-compatible transcription endpoint       |
| `stt_model`    | `whisper-1`                 | `model` form field                             |
| `stt_language` | `en`                        | language hint (`""` omits the field)           |
| `vad_silence_ms` | `700`                     | stop capture after this much continuous silence |
| `max_capture_ms` | `8000`                    | hard cap on one utterance                      |
| `publish_wake` | `1`                         | publish `tele/<name>/wake` on each trigger     |
| `play_tone`    | `1`                         | play start / connected / wake tone cues (set 0 to disable; the ES8311 itself is always brought up — see below) |
| `tts_url`      | `http://docker-host.mianos.com:8880/v1/audio/speech?sample_rate=16000` | FastKoko (Kokoro-FastAPI) speech endpoint for the `say` command |
| `tts_voice`    | `af_heart`                  | default voice; overridable per `say` call      |
| `mqtt_server`, `mqtt_port`, `sensor_name`, `tz` | mianos house defaults | broker / device identity |

**`stt_url` is empty by default and the device fails fast** (logs + publishes
`tele/<name>/stterror`) until you set it — nothing is hard-coded.

**Speaker output.** The ES8311 DAC is brought up once at boot, independent of
the mic init and non-fatal — if it can't be opened it logs and the mic/MQTT
path is unaffected (`play_tone`/`say` then just no-op). Two things use it:

- **Tone cues** (`play_tone`, default on): a two-note chime at boot, a
  three-note chime on first Wi-Fi connect, and a short blip on each wakeword.
  Wakeword capture is also shown on the RGB LED (green while capturing). Set
  `play_tone` to `0` to disable the cues (the DAC still comes up for `say`):
  ```
  mosquitto_pub -t 'cmnd/wsvoice/settings' -m '{"play_tone":0}'
  ```
- **`say` (TTS)** — speak arbitrary text via a FastKoko (Kokoro-FastAPI)
  server, over MQTT or HTTP:
  ```
  mosquitto_pub -t 'cmnd/wsvoice/say' -m '{"text":"hello there","voice":"af_heart"}'
  curl -X POST -d '{"text":"hello there"}' http://<ip>/say
  ```
  `voice` is optional (falls back to `tts_voice`). The device requests
  `response_format=pcm` — raw 16-bit samples, no container, no on-device
  decoding — and relies on the server to deliver audio already resampled to
  16 kHz mono via the `?sample_rate=16000` query param (a FastKoko extension;
  stock Kokoro-FastAPI is fixed at 24 kHz and can't do this itself). This
  matches the board's fixed 16 kHz shared I²S clock (mic + speaker on the same
  bus), so no resampling happens in firmware. A single utterance is capped at
  15 s of audio; a longer response is played up to the cap and logged as
  truncated.

Both features share one playback path (`bsp_audio_play_mono16`, serialised by
a mutex so a tone cue and a `say` can't collide) and publish `tele/<name>/tts`
/ `tele/<name>/ttserror` on completion/failure (see below).

The STT request is `multipart/form-data` with a `file` part (16-bit mono WAV),
`model` and optional `language` — compatible with faster-whisper-server,
speaches, whisper.cpp's server and the OpenAI `/v1/audio/transcriptions` API.

## MQTT topics

Commands (`cmnd/<name>/…`): `settings`, `say`, `restart`, `reprovision`.
Telemetry (`tele/<name>/…`):

| topic       | payload                                        |
|-------------|------------------------------------------------|
| `wake`      | `{"event":"wake"}` (each wakeword)             |
| `stt`       | `{"text":…, "ms":…, "stt_ms":…}` (transcript; `stt_ms` = round-trip latency) |
| `stterror`  | `{"error":…, …}` (unset URL, HTTP error, bad response) |
| `tts`       | `{"text":…, "ms":…, "tts_ms":…}` (speech played; `tts_ms` = round-trip latency) |
| `ttserror`  | `{"error":…, …}` (unset URL, HTTP error, empty response, speaker unavailable) |
| `init`      | version / build / ip, once on connect          |
| `status`    | uptime, heap, psram every 60 s                  |

## Web endpoints

`GET /healthz`, `POST /reset`, `POST /set_hostname` (mianos base) plus
`GET|POST /config`, `POST /config/reset`, `POST /say`, `GET|POST /firmware`
(raw-body OTA to the inactive slot; verified after reconnect, else rolled back).

## Tests

`test/host/` holds pure host unit tests for the WAV + multipart wire builders
(`main/SttWire.h`), the FastKoko request builder (`main/TtsRequest.h`), and the
tone synthesis (`main/ToneGen.h`) — the only host-testable units; the esp-sr /
I²S / codec path is verified on hardware.

```sh
bash test/host/run.sh
```

CI (`.gitlab-ci.yml`) runs those tests then builds the firmware under
`espressif/idf:release-v6.0`.

## Verifying end to end

1. `bash test/host/run.sh` — wire builders pass.
2. `./build.sh && idf.py -b 115200 flash monitor`; first boot provisions Wi-Fi
   via ESP-Touch v2.
3. Say **"Hi ESP"** (the phrase is fixed by the wn9_hiesp model — nothing else
   triggers it) → the RGB LED lights green, a short blip plays, the log shows
   the trigger, and `tele/wsvoice/wake` is published. The LED clears when
   capture ends.
4. Speak a phrase, pause → log shows capture length + VAD stop. With `stt_url`
   unset you get a `tele/wsvoice/stterror` (`stt_url_unset`) — the fail-fast.
5. Set `stt_url` (above) and repeat → `tele/wsvoice/stt {"text": …}` appears.
   Subscribe Node-RED to `tele/wsvoice/stt`.
6. `mosquitto_pub -t 'cmnd/wsvoice/say' -m '{"text":"testing one two three"}'`
   → the board speaks it and `tele/wsvoice/tts` appears.
