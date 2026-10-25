# Control API

[← Project overview](../README.md)

HTTP and USB serial call the same firmware handlers and return the same state object. Remote commands control the motor only in Remote mode; physical inputs control it in Local mode. Stop and status are available in either mode.

## Endpoints

| Path | HTTP method | Arguments | Result |
| --- | --- | --- | --- |
| `/` | GET | None | Embedded HTML interface |
| `/state` | GET | None | Applied state and input readings |
| `/set` | POST | Complete settings below | Apply settings in Remote mode |
| `/mode` | POST | `mode=remote` or `mode=local` | Stop outputs and select source |
| `/stop` | POST | None | Disconnect motor outputs and clear Local arming |

POST bodies use `application/x-www-form-urlencoded`. Successful control commands return the state JSON object. HTTP uses port 80. There is no application authentication or TLS in the firmware.

## Motor and pulse settings

`/set` requires every field below except `reverse`. The command is validated before settings are applied.

| Field | Allowed values | Meaning |
| --- | --- | --- |
| `carrier` | Integer 5000–20000 | Requested PWM switching frequency in Hz |
| `freq` | 0–1500 | Electrical frequency in Hz |
| `mod` | 0–100 | Modulation percentage |
| `dead` | 0.1–5 | Dead time in µs |
| `ton` | Integer 1–30000 | GPIO25 pulse high time in µs |
| `toff` | Integer 1–30000 | GPIO25 pulse low time in µs |
| `run` | `0` or `1` | Motor output enable request |
| `reverse` | `0` or `1`; optional | Phase direction; omitted retains the current direction |

Zero frequency or zero modulation leaves the motor outputs disabled even if `run=1`. Stop before requesting a different direction; a direction change while running is rejected. `/set` is rejected in Local mode, including changes to pulse timing or modulation.

The actual carrier is rounded to timer ticks. `actualCarrier` reports that value. At 20 kHz switching, 50 electrical Hz has about 400 updates per cycle; 1500 Hz has about 13. The accepted frequency range is a waveform-generator limit, rather than a motor-specific operating recommendation.

## HTTP examples

Replace `192.0.2.32` with your ESP32 address; it is a documentation placeholder.

```sh
# Read state, including the three input voltages.
curl http://192.0.2.32/state

# Apply settings with motor outputs disabled.
curl -X POST http://192.0.2.32/set \
  --data 'carrier=20000&freq=50&mod=10&dead=1&ton=20&toff=80&run=0&reverse=0'

# Select Local mode; this also stops the outputs.
curl -X POST http://192.0.2.32/mode --data 'mode=local'

# Stop from either control mode.
curl -X POST http://192.0.2.32/stop
```

## USB serial protocol

Use **115200 baud** and send newline-terminated requests:

```text
MC 1 /state
MC 2 /set?carrier=20000&freq=50&mod=10&dead=1&ton=20&toff=80&run=0&reverse=0
MC 3 /mode?mode=local
MC 4 /stop
```

The format is `MC <request-id> /path?key=value&...`. IDs are integers **1–999999999**. Arguments are URL-encoded. The parser accepts at most 20 arguments and 1536 command bytes; duplicate names, malformed encoding and invalid IDs are rejected. Carriage returns are ignored, so CRLF also works.

A response is a single line:

```text
MC {"id":4,"code":200,"data":{"running":false,"mode":"local",...}}
```

The ellipsis above abbreviates the state for readability; it is not part of an actual JSON response. Non-JSON errors are returned as strings in `data`. Boot messages do not have the `MC ` prefix. `/` is available over HTTP only.

## State fields

| Fields | Type / unit | Meaning |
| --- | --- | --- |
| `running` | Boolean | Six motor gate signals are connected to PWM |
| `mode` | `remote` / `local` | Current control source |
| `reverse` | Boolean | Applied phase direction |
| `freq`, `mod` | Hz, % | Applied electrical frequency and modulation |
| `carrier`, `actualCarrier` | Hz | Requested and rounded switching frequency |
| `dead` | µs | Requested dead time |
| `ton`, `toff` | µs | Independent pulse timing |
| `runSwitch`, `directionSwitch` | Boolean | Debounced physical contact states; closed = true |
| `localArmed` | Boolean | An OFF switch state has armed the next Local ON edge |
| `potRaw` | 0–4095 | Filtered potentiometer ADC reading |
| `runVoltage`, `directionVoltage`, `potVoltage` | V | Calibrated ADC estimates for GPIO32/33/34 |
| `localMaxHz` | Hz | Configured Local potentiometer maximum |
| `wifiConnected`, `ip` | Boolean, string | Wi-Fi connection state and current IP |

These are command/output and input-reading fields. They do not report measured motor speed, current or connector presence. `localArmed` can change between a command response and the next poll as inputs are serviced.

## Errors and delivery

| Code | Meaning |
| --- | --- |
| 200 | Command accepted; `data` contains state |
| 400 | Invalid settings, malformed serial request or invalid mode |
| 404 | Unknown serial command or HTTP route |
| 409 | `/set` requested in Local mode, or direction change requested while running |

Wi-Fi and serial have equal Remote authority: the last accepted command applies. The browser transports serialize requests and prioritize Stop over queued changes. Stop cancels queued `/set` and `/mode` actions, but cannot retract a command already applied by the ESP32.

A timeout means the outcome is unconfirmed. Commands are not automatically replayed. Read `/state` before retrying an action. Disconnecting a client does not stop the outputs; there is no communication watchdog.
