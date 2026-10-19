# ESP32 Motor Control

Classic ESP32, Arduino ESP32 core 3.3.12. Three complementary SVPWM phases with dead time, an independent GPIO25 pulse output, Wi-Fi HTTP and USB serial control, and local switches/potentiometer control.

## Wiring

| Function | GPIO | Connection |
| --- | --- | --- |
| U high / low | 18 / 19 | Gate driver inputs |
| V high / low | 21 / 22 | Gate driver inputs |
| W high / low | 23 / 26 | Gate driver inputs |
| Independent pulse | 25 | Existing pulse output |
| Run switch | 32 | Maintained switch to GND; closed = ON |
| Direction switch | 33 | Maintained switch to GND; closed = reverse |
| Potentiometer | 34 | Wiper; ends to 3.3 V and GND |

Switches use internal pull-ups and 30 ms debounce. GPIO34 uses ADC1 so it can be read while Wi-Fi operates. The 12-bit ADC reading is filtered and maps to 0–50 electrical Hz with a small zero deadband. Adjust `LOCAL_MAX_HZ` in the sketch to change the local range. Only 3.3 V belongs on these control inputs.

## Operation

Motor outputs boot OFF in Remote mode. The independent pulse generator runs as before. Copy `wifi_config.example.h` to `wifi_config.h` and enter your Wi-Fi credentials. The local file is ignored by Git. Without it, the firmware builds with placeholder credentials. Open the device address printed at 115200 baud to use its browser interface. Applied status refreshes every second. Edits remain in the form until applied; **Read settings into form** reloads applied values.

You can also open `motor-control.html` in a Web Serial capable browser (Chrome/Chromium) and click **Connect USB serial**, or enter the ESP32 HTTP address and connect over Wi-Fi. Web Serial requires a secure context such as localhost or a browser-supported local file. USB port opening may reset the ESP32. Use only one serial client at a time.

**Remote**: Wi-Fi and serial share validation and control authority; the last accepted command applies. Physical inputs are reported but do not control outputs.

**Local**: the potentiometer sets frequency; the switches set run and direction. Modulation, carrier and dead time retain their previously applied values. `/set` is rejected in Local mode. Set those values in Remote mode before switching to Local. Applying either control source stops the motor. After entering Local, the run switch must be observed OFF before an ON edge can start it. Changing direction while running stops the outputs and requires OFF then ON. A zero-frequency stop also requires a new OFF/ON cycle to restart.

**Stop** works through either interface in either mode. In Local mode, a held ON switch cannot restart the outputs after Stop. Stop disconnects all six motor gate signals; it does not stop GPIO25. Stopping outputs does not establish that the rotor has stopped.

Frequency and modulation are independent open-loop commands. There is no automatic induction-motor V/f profile, acceleration/deceleration ramp, current regulation, rotor-speed measurement or hardware fault input. Status reports commanded settings and gate-output enable state, rather than measured motor operation. Those functions and hardware waveform verification remain necessary work before operating a power stage with a motor.

## Shared HTTP / serial API

HTTP: `GET /state`; form-encoded `POST /set`, `/mode`, `/stop`.

Serial: 115200 baud, newline-terminated requests:

```text
MC 1 /state
MC 2 /set?carrier=20000&freq=50&mod=10&dead=1&ton=20&toff=80&run=0&reverse=0
MC 3 /mode?mode=local
MC 4 /stop
```

Responses have the form `MC {"id":1,"code":200,"data":{...}}`. Boot logs have no `MC ` prefix. Arguments are URL-encoded. IDs are integers 1–999999999. Requests are bounded to 1536 bytes; malformed, duplicate and invalid serial arguments are rejected.

`/set` requires all the original motor/pulse settings and `run=0|1`; `reverse=0|1` is optional and defaults to the current direction. Stop before requesting a different direction. Accepted settings return the same JSON object as `/state`; invalid settings return 400, and mode/direction conflicts return 409, without applying the command.

Limits: carrier 5000–20000 integer Hz; electrical frequency 0–1500 Hz; modulation 0–100%; dead time 0.1–5 µs; pulse on/off times 1–30000 integer µs each. Zero frequency or modulation disables motor outputs. Actual carrier frequency is rounded to hardware ticks. High electrical frequencies have fewer PWM samples per waveform cycle.

The browser serial and HTTP transports serialize requests and give Stop priority over queued changes. They do not automatically replay unconfirmed commands. Losing a client connection leaves the last output command active; there is no communication watchdog.

## Build and checks

```sh
python3 web/build.py
arduino-cli compile --fqbn esp32:esp32:esp32 .
node --test tests/*.mjs
python3 tests/test_local_controls.py
```

Edit `web/index.html`, `web/serial.mjs` and `web/http.mjs`; the build script generates `web_page.h` and the standalone `motor-control.html`. Transport tests use simulated serial/HTTP peers. The local-control check compiles the actual firmware input state machine against simulated GPIO/ADC/time; it requires `g++`. These checks do not validate physical signals or motor behavior. No firmware upload is performed by these commands.

Protocol helpers and browser transports were adapted from the sibling ESP32 Function Generator project; see `THIRD_PARTY_NOTICES.md`.
