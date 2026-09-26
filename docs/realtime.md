# Satellite1 realtime voice (OpenAI Realtime)

This is an **opt-in, experimental** Satellite1 firmware variant. It connects the
on-device wake word and microphone to a Home Assistant add-on that bridges to
OpenAI Realtime and Home Assistant's MCP Server. Unlike the stock Satellite1
voice firmware, it does **not** use Home Assistant's `voice_assistant` pipeline.
Speech leaves your home for OpenAI; check its pricing and data policies before
enabling the add-on. On-device audio, echo cancellation, and interruption still
need hardware qualification; use the [checklist](#hardware-bring-up-checklist)
before treating this as a reliable daily driver.

```text
Satellite1 mic + XMOS AEC -- WebSocket (va_url) --> HAOS realtime add-on
         wake word / PCM                               |        |
                                                      |        +--> OpenAI Realtime
                                                      +--> HA MCP Server --> exposed HA entities

Satellite1 audio output:
  announcements ---------------------> [announcement lane] --+
  music / media / Sendspin ----------> [media lane] ---------+--> 3-lane mixer --> speaker / line-out
  realtime assistant PCM ------------> [assistant lane] -----+
                                          (media ducks during voice/announcements)
```

The assistant has its own volume control and mixer input. Regular Home
Assistant announcements and the media player (including Music Assistant via
Sendspin) remain available.

## Before you start

- A Satellite1 Core Board with microphone HAT, USB-C cable, and a Home
  Assistant OS installation that supports add-ons. ESPHome Device Builder is
  useful for subsequent OTA updates. Allow access from the Satellite1 to the
  add-on's WebSocket port (default `8080`) on your LAN.
- An OpenAI API key with **billing enabled**. Configure it in the add-on,
  not in the firmware YAML. API use may incur charges.
- The **Home Assistant MCP Server integration**, with the entities you want
  the assistant to control explicitly exposed in Home Assistant. Check the
  integration and exposure settings before testing home-control requests.
- A fork containing both this realtime firmware and the compatible realtime
  add-on. The example fork URLs below are **placeholders**, not published
  release links. The firmware build uses ESPHome `2026.8.1` in this branch;
  use a compatible ESPHome Device Builder for dashboard builds.

## 1. Install and configure the add-on

Choose **one** installation route:

1. **Repository:** In Home Assistant, go to **Settings → Add-ons → Add-on
   store → ⋮ → Repositories** and add
   `https://github.com/<your-fork>/ha-openai-realtime`. Find the realtime
   voice add-on in the store and install it. Replace `<your-fork>` with a fork
   that actually contains the add-on.
2. **Local add-on:** Copy the add-on's `openai_realtime_voice_agent` folder
   (including its `config.yaml`) into
   `/addons/openai_realtime_voice_agent` on the HAOS host via Samba/SSH.
   Then use **Add-on store → ⋮ → Check for updates → Local add-ons**,
   install the add-on, and open its configuration.

Install the HA MCP Server integration and expose only the entities the
assistant should access. In the add-on configuration, enter your
`openai_api_key`. The add-on documentation recommends leaving `ha_mcp_url`
blank for the standard HAOS setup; set a custom URL only if your installation
requires one. Keep `websocket_port: 8080` unless you also change the firmware
URL. Start the add-on and check its logs for startup errors. The add-on
option `interrupt_response` defaults to **off**; leave it off for initial
bring-up.

The add-on currently shares one conversation/pipeline across connections:
**use one Satellite1 per add-on instance**. Do not treat multiple connected
devices as isolated users.
On each WebSocket connection, firmware sends a `start` message with the
lowercase base Wi-Fi MAC (`mac`) and ESPHome node name (`name`).

## 2. Build and flash the firmware

For a local build, use `config/satellite1.realtime.yaml`, **not**
`config/satellite1.yaml`. Edit its `va_url` substitution to the add-on's
reachable LAN address, for example `ws://192.168.1.20:8080/` (the default is
`ws://homeassistant.local:8080/`). If needed, select microphone channel
`va_mic_channel` (`0` or `1`). Do not put the OpenAI key in this file.
No secrets file is needed: Wi-Fi comes from Improv provisioning and the
ESPHome API encryption key is provisioned by Home Assistant.

From the repository root, with the project's ESPHome environment installed:

```sh
source scripts/setup_build_env.sh
esphome compile config/satellite1.realtime.yaml
```

Alternatively, build in Docker (for example, on macOS):

```sh
docker run --rm -v "$PWD":/config -w /config \
  ghcr.io/esphome/esphome:2026.8.1 compile config/satellite1.realtime.yaml
```

**Which flash method?** A Satellite1 already running stock FPH firmware
accepts **ESPHome OTA over Wi-Fi** (the stock build includes `ota: esphome`),
so USB is not required. Wi-Fi credentials (from Improv) and the Home
Assistant-provisioned API key are kept across the update, so the device
stays in Home Assistant under the same name. Use USB only for a blank or
bricked device.

**OTA from a Docker build on the LAN** (replace the IP with the device's):

```sh
docker run --rm -v "$PWD":/config -w /config \
  ghcr.io/esphome/esphome:2026.8.1 upload config/satellite1.realtime.yaml \
  --device 192.168.1.50
```

**USB (blank or bricked devices):** open [web.esphome.io](https://web.esphome.io/)
in a compatible browser on the machine connected to Satellite1, connect over
USB, and install the locally compiled factory image at
`config/.esphome/build/satellite1/build/firmware.factory.bin`. Alternatively,
use host-installed `esptool` to write that factory image at address `0x0`
on the ESP32-S3. **Docker Desktop on macOS cannot pass USB serial through to
the ESPHome container**, so use Docker to compile and the browser or host
`esptool` to flash.

For a new device, provision Wi-Fi through Home Assistant's Improv BLE flow
and authorize provisioning with the action button (or use your existing Wi-Fi
configuration). Wait for Wi-Fi and the ESPHome API to connect, then add/adopt
the device in Home Assistant. The local build can automatically flash the
embedded XMOS firmware on first boot when needed; let that finish before
testing audio. Keep a USB recovery path available.

### ESPHome Device Builder with local files (no fork needed)

To build and install from Home Assistant's ESPHome Device Builder app using
this checkout, stage the files and copy them into the Builder's config
directory (`/config/esphome` on the HA host, via the Samba or SSH app):

```sh
scripts/stage_realtime_builder.sh /tmp/sat1-builder satellite1-a1b2c3 ws://192.168.1.20:8080/
```

Use your device's existing name (as shown in Builder/Home Assistant) so the
update replaces it in place. The script writes a small device YAML named
after the device, the two `satellite1.realtime*.yaml` files, `common/` and
`components/`. **Back up the device's existing YAML first**; the new one
replaces it. If that old YAML set explicit `api`, `ota` or `wifi` keys, copy
them into the new one. Then in Builder choose the device → **Install** →
**Wirelessly**. Builder must be ESPHome ≥2026.7.0; the first build on a
low-power HA host can take a long time. `satellite1.realtime.yaml` and
`satellite1.realtime.base.yaml` also appear as devices in Builder; ignore them.
To go back to stock, restore the old YAML and install it wirelessly.

### ESPHome Device Builder / dashboard stub (published fork)

For Builder-managed OTA updates, copy
`config/satellite1.realtime.dashboard.yaml` into your ESPHome Device Builder
device YAML. Set `realtime_repo_url` to your **firmware fork** and
`realtime_repo_ref` to a ref containing *both* the realtime packages and
`esphome/components/va_client`; the default FutureProofHomes URL/ref is only
a placeholder until publication. Set `va_url` to your reachable add-on
WebSocket URL (and `va_mic_channel` if necessary), then supply the device's
normal Wi-Fi/API settings or adopt its existing keys in Builder. Compile and
install wirelessly. Dashboard builds wait for a
responding XMOS rather than automatically flashing it; the dashboard exposes
an **XMOS Flash Embedded FW** button for manual firmware flashing.

This variant deliberately omits the FPH HTTP OTA update manifest/provider:
stock Satellite1 updates will not automatically overwrite it. Keep your
realtime YAML and fork/ref current to receive ESPHome OTA updates.

## 3. Use it

Say **"Okay Nabu"** or **"Hey Jarvis"** to start a conversation. The action
button starts a session when idle; during an active session it interrupts,
during an announcement it stops the announcement, and during media playback
it pauses the media. The internal **"stop"** wake word interrupts only after
a reply has started playing. Add-on follow-up listening is configurable; a
new wake word during a reply interrupts it and starts a new session.

Useful Home Assistant controls from this firmware:

| Entity name | Purpose |
| --- | --- |
| **Hands-free interrupt** | Lets speech interrupt a reply; off by default. Requires the add-on option below too. |
| **Barge-in holdoff** | Mic-transmit pause after reply playback starts (0–2000 ms; default 400 ms). |
| **Assistant voice level** | Assistant-only output level (0.3–1.0, default 1.0). |
| **Mute Microphones** | Software mic mute; hardware mute takes precedence. Muting stops the current assistant session and closes follow-up listening. |
| **Wake sound** | Toggle the wake chime. |
| **Wake word sensitivity** | Slightly, Moderately, or Very. |
| **Media Player** | Media volume, playback, announcements; muting it also silences assistant output. |
| **Speaker channel output**, **Line-Out Connected** | Output selection/status for speaker and jack. |
| **LED Ring**, **Restart Sat1**, **USB-C Power Supply**, **XMOS Firmware** | Device controls and diagnostics. |

The firmware ducks the **media** mixer input by 20 dB immediately while
assistant activity, follow-up, queued assistant audio, or an announcement is
present, then restores it over about one second. Music keeps its own media
lane; the announcement and assistant lanes are not themselves ducked. Check
restoration in your own Music Assistant / Sendspin setup, including the
follow-up window. Media volume and assistant voice level are separate;
media-player **mute** applies to both.

### Hands-free interruption (barge-in)

After initial playback works, set `interrupt_response: true` in the add-on
and restart it, **then** turn on the Satellite1 **Hands-free interrupt**
switch. Both controls must be on: if only the device switch is on, look for
`hands-free interrupt is on but add-on interrupt_response is off` in the log.
**Barge-in holdoff** (default 400 ms) suppresses mic transmission until
that long after the first real reply audio is fed to the speaker; it resets
for each reply, including replies after tool calls. On server interruption,
the queued TTS fades out over 10 ms and late PCM from the cancelled response
is dropped until the next turn. Look for `barge-in: faded out`,
`barge-in holdoff`, and the dropped-straggler count in device logs.

XMOS acoustic echo cancellation is imperfect; loud speech or music can cause
false interrupts. Try `noise_reduction: far_field` in the add-on and leave
hands-free interrupt off until the [AEC qualification](#hardware-bring-up-checklist)
passes. The realtime-only `i2s_buffer_duration` substitution defaults to
`500ms`; a smaller value shortens the audible tail after an interrupt or
"stop" but can stutter music. Test `200ms` with Sendspin before keeping it.

**Timers:** with add-on ≥0.6.1-sat1.4 (`enable_timers` on), ask to set,
list or cancel timers (up to 8, 1 s–24 h). Timers live on the Satellite1:
they keep running and ring even if the add-on disconnects, and are re-synced
on reconnect, but are lost on reboot. The LED ring shows timer progress.
Stop a ringing timer with the "stop" wake word, the center button, or the
**Stop Timer Ringing** button in Home Assistant; ringing auto-stops after
15 minutes. Timers are untested on hardware in this preview.

## Troubleshooting

1. **No connection / no response:** Confirm the add-on is running and read
   its logs. Make sure `va_url` is a `ws://` URL with the right LAN host and
   port (default `ws://homeassistant.local:8080/`), resolvable *from the
   device*, not an add-on-only hostname. Check Wi-Fi, ESPHome API, and the
   WebSocket port; consult ESPHome device logs for connection failures.
2. **No home control:** Confirm the HA MCP Server integration is installed,
   the desired entities are exposed, and the add-on has a valid OpenAI key
   with billing. Read add-on logs for MCP/OpenAI errors rather than assuming
   an audio failure.
3. **No sound / interrupted reply:** Check hardware and software mic mute,
   media-player mute, **Assistant voice level**, selected output and
   line-out jack. Try disabling **Hands-free interrupt** and
   `interrupt_response`; reduce voice/media level and re-test echo. For
   add-on-side audio diagnosis, `enable_recording: true` writes input/output
   WAVs to `/share/openai_realtime_voice_agent/recordings/` (add-on
   0.6.1-sat1.6+); turn it off when finished and handle recordings as
   sensitive audio.
4. **Follow-up answers not heard:** Set the add-on `transcription_language`
   (for example `en`) so the log shows `🗣️ user:` lines, and enable
   recording to hear what the backend received. If the input WAV is quiet,
   raise **Assistant mic gain** (config entity, 1–16, default 1; watch for
   clipping). **Assistant mic channel** switches the XMOS channel streamed
   to the backend (`0` is the processed ASR/AEC channel used by stock Assist)
   without reflashing. Very loud replies can leave the echo canceller
   suppressing the mic for a moment afterwards; test at a moderate volume
   and try `noise_reduction: far_field`. The firmware options are
   `va_mic_gain` / `va_mic_channel` substitutions; the HA entities restore
   their last value and override them after the first boot.
5. **Choppy replies:** Firmware from this revision primes the playback
   buffer once per reply instead of re-priming mid-reply. If replies still
   stutter, raise the add-on `playback_prebuffer_ms` (for example 300) and
   check device logs for `ws audio gap` and `downstream underrun` warnings.
6. **LED ring:** Blue progress indicates XMOS flashing; a green/red pulse
   indicates success/failure. Slow clockwise light means waiting, fast
   clockwise means listening, pulsing means thinking, and fast anticlockwise
   means replying. Red twinkle can mean lost Wi-Fi/API/WebSocket connectivity,
   a component failure, or not-ready; check logs to distinguish them. Red
   LEDs near the mics/speaker indicate mute/silence when no higher-priority
   phase animation is active. Idle is normally dark.
7. **Clicks at the start/end of replies:** The TAS2780 amp's noise gate
   powers the Class-D stage down after 50 ms of digital silence and back up
   when audio returns. The realtime variant turns it off by default; the
   **Speaker noise gate** config switch re-enables it (slightly lower idle
   power, but clicks return). Clicks *between sentences* come from the
   add-on's stream pausing. The firmware fades out only when its estimate of
   the queued playback runs low, and fades the next audio back in. If
   `ws audio gap: … ~N ms queued downstream` shows N near 0, raise the
   add-on's `playback_prebuffer_ms`.
8. **Boot loop or no logs over Wi-Fi:** The HAT's USB-C port only supplies
   power. Connect the Core board's **CORE/ESP32** USB-C port instead; it
   exposes the ESP32-S3 USB-Serial/JTAG console used by `logger`. Read it
   with <https://web.esphome.io> ("Logs"), or with pyserial opened with
   `dtr=False, rts=False` (asserting them holds the chip in reset). To
   recover, hold BOOT while tapping RESET, then flash from the host (Docker
   on macOS can't reach USB devices):
   `esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600
   write-flash 0x0 .esphome/build/<name>/build/firmware.factory.bin`.
   The factory image keeps the NVS partition, so Wi-Fi credentials survive.
9. **Internal RAM is tight:** The realtime variant moves its task stacks to
   PSRAM so the microphone's I2S DMA buffers still fit at boot while BLE is
   enabled. Keep `web_server` out of the device YAML unless the `debug`
   component shows spare internal heap.

## Hardware bring-up checklist

This is an **owner-run on-device qualification plan**, not a claim that the
hardware has already passed. Record the firmware and add-on versions,
`va_url`, output route and approximate room/noise conditions for each run.

1. **Wake, reply, stop, button, follow-up, reconnect:** Verify each wake
   word produces a complete spoken reply; say "stop" after reply audio
   begins; use the action button to start and interrupt sessions. Exercise
   add-on-configured follow-up listening, then restart the add-on or briefly
   drop Wi-Fi and verify reconnect and a fresh reply without reflashing.
2. **Music and announcements:** Play Music Assistant music via Sendspin.
   Start voice turns and follow-ups, trigger a Home Assistant announcement,
   and check media ducks promptly and returns to its former audible level
   after each phase. Check what happens when pausing with the action button,
   and repeat with speaker and jack/line-out connected (confirm output
   selection and jack status).
3. **Mute in every phase:** Toggle software mic mute and the hardware mute
   while idle, listening, thinking, replying and during follow-up; verify
   sessions stop and muted audio is not captured. Check that hardware mute
   still wins when software mute is off. Mute the media player during a reply
   and verify assistant output goes silent too; restore both controls.
4. **Volume and trim:** Sweep **Assistant voice level** (0.3–1.0) separately
   from media volume. Check the volume buttons and any external line-out/amp
   trim, clipping and headroom on speaker and line-out; compare with/without
   music and announcements. Keep output at comfortable levels for the next
   test.
5. **Runtime memory:** Temporarily enable the commented `common/debug.yaml`
   package in `config/satellite1.realtime.base.yaml`, rebuild/OTA, and
   monitor ESPHome logs for free heap/fragmentation through idle, extended
   replies, media, repeated follow-ups and reconnects. Record whether memory
   recovers after each cycle or trends downward; turn the package back off
   for normal operation.
6. **AEC and barge-in qualification:** Only after the preceding tests,
   enable `enable_recording` in the add-on (input/output WAVs), enable both
   `interrupt_response` and **Hands-free interrupt**, choose a
   **Barge-in holdoff**, and record at least 20 unattended TTS replies
   **at each** low, medium and maximum comfortable voice level, both with
   and without Sendspin music. Count false interrupts where playback itself
   triggers barge-in. At each condition, make at least 20 deliberate
   interruptions while TTS plays and count missed/late ones; repeat with a
   shorter I2S buffer (for example `200ms`) and check music for stutter.
   Inspect WAVs for echo leaking into the mic and repeat with speaker and
   line-out as appropriate. A provisional pass criterion is **zero false
   interruptions per 20 replies** (fewer than one) and **at most one missed
   interruption per 20 attempts** at every condition. If it fails, lower
   **Assistant voice level** and media volume, adjust speaker placement or
   output route, check XMOS AEC and mic channel, and repeat the same counts;
   leave hands-free interrupt disabled if it remains unreliable. Disable
   WAV recording afterward and retain/share recordings only with consent.

## Licensing and credits

The `va_client` C++ component is GPLv3 code ported from
[xandervanerven's Voice PE fork](https://github.com/xandervanerven/home-assistant-voice-pe),
building on [maxmaxme's fork](https://github.com/maxmaxme/home-assistant-voice-pe).
Transport/microphone fixes draw from the **sgorilla** and **HaipeiWang**
Voice PE forks; backend fixes were ported from **kyvaith**'s
`ha-openai-realtime` fork. Credit also goes to ESPHome, FutureProofHomes,
Home Assistant, and the add-on's upstream maintainers. Keep the applicable
component licenses and upstream notices when redistributing a build.
