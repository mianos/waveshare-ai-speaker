# ws-voice

Voice control bridge for a **Waveshare ESP32-S3 audio board**. On the wakeword
**"Computer"** (detected on-device by esp-sr / WakeNet9) it records the following
speech, POSTs it to a configurable Whisper-style speech-to-text server, and
publishes the transcript to MQTT for Node-RED (or anything else) to act on.

```
mic (ES7210) ──▶ esp-sr AFE ──▶ WakeNet "Computer" ──▶ capture (VAD-ended)
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
| `play_url`     | `http://mqtt2.mianos.com/pcm/plucky.pcm` | default raw-PCM cue clip for the `play` command |
| `wakenet_mode` | `-1` (esp-sr default)       | WakeNet sensitivity: `0`/`1`=90%/95% normal, `2`/`3`=2-channel 90%/95% (this board's array), `4`/`5`=3-channel. Higher = more sensitive, more false triggers |
| `vad_mode`     | `-1` (esp-sr default)       | VAD aggressiveness `0`-`4` (`0`=normal…`4`=very very very aggressive); *lower* reports speech more readily |
| `agc_target_dbfs` | `-1` (esp-sr default, `3`) | AGC target envelope in -dBFS |
| `agc_compression_db` | `-1` (esp-sr default, `9`) | AGC fixed digital compression gain, dB |
| `mic_gain_x100` | `100`                      | extra linear *output* gain (post-AFE), ×100 (`100`=1.00x, valid `10`-`1000` = 0.1x-10x) — boosts noise along with signal, doesn't improve pickup SNR |
| `mic_hw_gain_db` | `30`                       | ES7210 analog mic PGA gain in dB, applied at the ADC before any processing (real hardware gain — this is the one that improves pickup at distance); quantised to the codec's supported steps, up to 37.5dB |
| `mic_level_log` | `0`                        | `1` ⇒ log each mic channel's RMS/peak (dBFS) for each wake capture, for aiming/gaining the array |
| `mqtt_server`, `mqtt_port`, `sensor_name`, `tz` | mianos house defaults | broker / device identity |

**`stt_url` is empty by default and the device fails fast** (logs + publishes
`tele/<name>/stterror`) until you set it — nothing is hard-coded.

**Speaker output.** The ES8311 DAC is brought up once at boot, independent of
the mic init and non-fatal — if it can't be opened it logs and the mic/MQTT
path is unaffected (`play_tone`/`say` then just no-op). Two things use it:

- **Tone cues** (`play_tone`, default on): a two-note chime at boot, a
  three-note chime on first Wi-Fi connect, and a short blip on each wakeword.
  Once MQTT actually connects (a separate, stronger signal than the Wi-Fi
  chime — the broker can be unreachable even with an IP) it also speaks
  "Connected" via TTS, using the same `play_tone` gate and `tts_url`/`tts_voice`
  as `say` below. Wakeword capture is also shown on the RGB LED (green while
  capturing). Set `play_tone` to `0` to disable all of the cues (the DAC
  still comes up for `say`):
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
- **`play` (cue clips)** — fetch and play a pre-converted raw-PCM clip by URL,
  over MQTT or HTTP:
  ```
  mosquitto_pub -t 'cmnd/wsvoice/play' -m '{"url":""}'              # default (play_url)
  mosquitto_pub -t 'cmnd/wsvoice/play' -m '{"url":"http://mqtt2.mianos.com/pcm/plucky.pcm"}'
  curl -X POST -d '{}' http://<ip>/play                             # default (play_url)
  ```
  Over MQTT the payload must be *non-empty* JSON (`{"url":""}` for the
  default) — the shared MQTT wrapper drops a bare `{}` before dispatch. The
  HTTP route has no such constraint: an empty or `{}` body plays the default.
  Like `say`, the device does no decoding or resampling — the URL must serve
  the board's native format: **raw 16 kHz mono s16le, no container**. Unlike
  the TTS stream, the whole clip is buffered into PSRAM (capped at 2 MB ≈ 64 s)
  before playback starts, so a mid-download Wi-Fi stall can't punch a gap in
  the sound. Convert an MP3 on the web host (`/var/www/html/pcm/`) with:
  ```
  ffmpeg -i clip.mp3 -af "volume=0.45,adelay=100" -ar 16000 -ac 1 -f s16le pcm/clip.pcm
  ```
  `adelay=100` prepends 100 ms of silence to cover the speaker amp's bias-up
  (an attack at sample 0 gets swallowed — same reason the wake blip carries a
  lead-in); tune `volume` per file so the peak lands near the tone amplitude
  (~12000) rather than the int16 rails.

Both features share one playback path (`bsp_audio_play_mono16`, serialised by
a mutex so a tone cue and a `say` can't collide) and publish `tele/<name>/tts`
/ `tele/<name>/ttserror` on completion/failure (see below).

**Near-field mic array tuning.** The board's 2-mic array feeds esp-sr's AFE
(AEC → SE/BSS → NS → VAD → WakeNet). `wakenet_mode`/`vad_mode`/`agc_target_dbfs`/
`agc_compression_db`/`mic_gain_x100`/`mic_hw_gain_db` (above) tune sensitivity
and gain without a rebuild — `mic_hw_gain_db` takes effect immediately (a live
ES7210 register write); the AFE-side settings (`wakenet_mode`, `vad_mode`,
`agc_*`, `mic_gain_x100`) are baked into the AFE at boot, so those need a
reboot (`cmnd/<name>/restart`) to apply. The `-1` defaults on the AFE settings
leave esp-sr's own input-format-derived choices alone. To dial these in, turn
on the level meter and watch the console while speaking at the mic's normal
distance/angle:
```
mosquitto_pub -t 'cmnd/wsvoice/settings' -m '{"mic_level_log":1}'
```
This logs one summary line per wake capture (not on a free-running timer —
those samples mostly land on ambient noise between events, not the recording
itself), covering the whole utterance from wake to end-of-speech: `capture
levels (2310ms): mic0 rms=-22.1dB peak=-8.4dB  mic1 rms=-24.0dB peak=-9.1dB`.
It's raw mic data (before AFE processing) — useful for checking both mics see
comparable levels and aren't clipping (peak near 0 dBFS) or too quiet. If it's
too quiet, raise `mic_hw_gain_db` first (real hardware gain, up to ~37.5dB) —
`mic_gain_x100`/`agc_target_dbfs` only scale the AFE's already-processed
*output* and boost noise right along with the signal, so they don't fix a
genuine pickup/SNR shortfall. Once levels look healthy, tune `wakenet_mode`
for the false-trigger vs. missed-wake tradeoff.

The STT request is `multipart/form-data` with a `file` part (16-bit mono WAV),
`model` and optional `language` — compatible with faster-whisper-server,
speaches, whisper.cpp's server and the OpenAI `/v1/audio/transcriptions` API.

## MQTT topics

Commands (`cmnd/<name>/…`): `settings`, `say`, `play`, `timer`, `volume`,
`restart`, `reprovision`.
Telemetry (`tele/<name>/…`):

| topic       | payload                                        |
|-------------|------------------------------------------------|
| `wake`      | `{"event":"wake"}` (each wakeword)             |
| `stt`       | `{"text":…, "ms":…, "stt_ms":…}` (transcript; `stt_ms` = round-trip latency) |
| `stterror`  | `{"error":…, …}` (unset URL, HTTP error, bad response) |
| `tts`       | `{"text":…, "ms":…, "tts_ms":…}` (speech played; `tts_ms` = round-trip latency) |
| `ttserror`  | `{"error":…, …}` (unset URL, HTTP error, empty response, speaker unavailable) |
| `play`      | `{"url":…, "ms":…, "http_ms":…}` (clip played; `http_ms` = fetch latency) |
| `playerror` | `{"error":…, …}` (unset URL, HTTP error, too large, empty, speaker unavailable) |
| `init`      | version / build / ip, once on connect          |
| `status`    | uptime, heap, psram every 60 s                  |

## Web endpoints

`GET /healthz`, `POST /reset`, `POST /set_hostname` (mianos base) plus
`GET|POST /config`, `POST /config/reset`, `POST /say`, `POST /play`,
`GET|POST /volume`, `GET|POST /firmware` (raw-body OTA to the inactive slot;
verified after reconnect, else rolled back).

## Tests

`test/host/` holds pure host unit tests for the WAV + multipart wire builders
(`main/SttWire.h`), the FastKoko request builder (`main/TtsRequest.h`), the
tone synthesis (`main/ToneGen.h`), and the mic level meter maths
(`main/MicLevel.h`) — the only host-testable units; the esp-sr / I²S / codec
path is verified on hardware.

```sh
bash test/host/run.sh
```

CI (`.gitlab-ci.yml`) runs those tests then builds the firmware under
`espressif/idf:release-v6.0`.

## Verifying end to end

1. `bash test/host/run.sh` — wire builders pass.
2. `./build.sh && idf.py -b 115200 flash monitor`; first boot provisions Wi-Fi
   via ESP-Touch v2.
3. Say **"Computer"** (the phrase is fixed by the wn9_computer_tts model —
   nothing else triggers it) → the RGB LED lights green, a short blip plays, the log shows
   the trigger, and `tele/wsvoice/wake` is published. The LED clears when
   capture ends.
4. Speak a phrase, pause → log shows capture length + VAD stop. With `stt_url`
   unset you get a `tele/wsvoice/stterror` (`stt_url_unset`) — the fail-fast.
5. Set `stt_url` (above) and repeat → `tele/wsvoice/stt {"text": …}` appears.
   Subscribe Node-RED to `tele/wsvoice/stt`.
6. `mosquitto_pub -t 'cmnd/wsvoice/say' -m '{"text":"testing one two three"}'`
   → the board speaks it and `tele/wsvoice/tts` appears.
