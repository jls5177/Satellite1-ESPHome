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

## 2. Build and flash the firmware

For a local build, use `config/satellite1.realtime.yaml`, **not**
`config/satellite1.yaml`. Edit its `va_url` substitution to the add-on's
reachable LAN address, for example `ws://192.168.1.20:8080/` (the default is
`ws://homeassistant.local:8080/`). If needed, select microphone channel
`va_mic_channel` (`0` or `1`). Do not put the OpenAI key in this file.
Provide the ESPHome API encryption key and any network credentials required
by your build without committing secrets to the fork.

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

For the **first USB flash**, open [web.esphome.io](https://web.esphome.io/)
in a compatible browser on the machine connected to Satellite1, connect over
USB, and install the locally compiled factory image at
`config/.esphome/build/satellite1/build/firmware.factory.bin`. Alternatively,
use host-installed `esptool` to write that factory image at address `0x0`
on the ESP32-S3, or run `esphome run config/satellite1.realtime.yaml` with
USB serial access on a supported host. **Docker Desktop on macOS cannot
pass USB serial through to the ESPHome container**: use Docker to compile,
then use the browser or host `esptool` to flash. Do not assume that `esphome
run` *inside Docker on macOS* can perform the USB step.

For a new device, provision Wi-Fi through Home Assistant's Improv BLE flow
and authorize provisioning with the action button (or use your existing Wi-Fi
configuration). Wait for Wi-Fi and the ESPHome API to connect, then add/adopt
the device in Home Assistant. The local build can automatically flash the
embedded XMOS firmware on first boot when needed; let that finish before
testing audio. After the first USB flash, updates can use **ESPHome OTA**
(`esphome run` on the LAN or ESPHome Device Builder). Keep a USB recovery
path available.

### ESPHome Device Builder / dashboard stub

For Builder-managed OTA updates, copy
`config/satellite1.realtime.dashboard.yaml` into your ESPHome Device Builder
device YAML. Set `realtime_repo_url` to your **firmware fork** and
`realtime_repo_ref` to a ref containing *both* the realtime packages and
`esphome/components/va_client`; the default FutureProofHomes URL/ref is only
a placeholder until publication. Set `va_url` to your reachable add-on
WebSocket URL (and `va_mic_channel` if necessary), then supply the device's
normal Wi-Fi/API settings or adopt its existing keys in Builder. Compile and
install by OTA after the initial USB flash. Dashboard builds wait for a
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
switch. Both must be enabled: the firmware requires server consent as well
as its own switch. When enabled, the mic continues streaming while the
assistant thinks/speaks; speech can interrupt a reply. XMOS acoustic echo
cancellation is imperfect, so the assistant's own loud speech or music can
cause false interrupts. Leave hands-free interrupt off until the
[AEC qualification](#hardware-bring-up-checklist) passes.

**Timers:** coming with add-on ≥0.6.1-sat1.3 + firmware timer support; do
not rely on timer functionality in this preview.

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
   WAVs to its `recordings/` directory; turn it off when finished and handle
   recordings as sensitive audio.
4. **LED ring:** Blue progress indicates XMOS flashing; a green/red pulse
   indicates success/failure. Slow clockwise light means waiting, fast
   clockwise means listening, pulsing means thinking, and fast anticlockwise
   means replying. Red twinkle can mean lost Wi-Fi/API/WebSocket connectivity,
   a component failure, or not-ready; check logs to distinguish them. Red
   LEDs near the mics/speaker indicate mute/silence when no higher-priority
   phase animation is active. Idle is normally dark.

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
   `interrupt_response` and **Hands-free interrupt**, and record at least
   20 unattended TTS replies **at each** low, medium and maximum comfortable
   voice level, both with and without music. Count false interrupts where
   playback itself triggers barge-in. At each condition, make at least 20
   deliberate interruptions while TTS plays and count missed/late ones;
   inspect WAVs for echo leaking into the mic and repeat with speaker and
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
