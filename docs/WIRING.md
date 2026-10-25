# Wiring & inputs

[← Project overview](../README.md)

## Signal connections

| Function | GPIO | Connection / meaning |
| --- | --- | --- |
| U high-side / low-side | 18 / 19 | First phase gate-driver inputs |
| V high-side / low-side | 21 / 22 | Second phase gate-driver inputs |
| W high-side / low-side | 23 / 26 | Third phase gate-driver inputs |
| Independent pulse | 25 | Separate pulse-generator output |
| Run switch | 32 | Maintained switch to GND; closed = ON |
| Direction switch | 33 | Maintained switch to GND; closed = reverse |
| Potentiometer | 34 | Wiper; end terminals to 3.3 V and GND |

These are ESP32 logic connections. The gate-driver and power-stage design is separate from this firmware. Use a suitable gate-driver interface and signal reference for your hardware. Only 3.3 V belongs on the control inputs; do not connect motor or DC-bus voltage to an ESP32 pin.

## Physical controls

```text
ESP32 GPIO32 ───── run switch ───── GND
ESP32 GPIO33 ── direction switch ── GND

                     potentiometer
3.3 V ───────────── /\/\/\/\/\ ───────────── GND
                         ↑
                       wiper
                         │
                    ESP32 GPIO34
```

GPIO32 and GPIO33 use the ESP32's internal pull-ups. An open contact reads inactive; a closed contact pulls the pin LOW. Both switches use 30 ms debounce. A momentary pushbutton is not currently interpreted as a latched toggle.

The potentiometer uses a 12-bit ADC reading, filtered with an exponential moving average. Its default Local range is **0–50 electrical Hz**. A filtered reading below 40 counts is treated as zero. `LOCAL_MAX_HZ` in the sketch sets the upper frequency. Frequency is updated when its change reaches 0.1 Hz, or when it reaches zero.

## Analog voltage readings

All three inputs use ADC1 channels, which allow analog readings alongside Wi-Fi. The firmware calls `analogReadMilliVolts()` with 11 dB attenuation and exposes these readings in `/state`:

| Field | Pin | Unit | Meaning |
| --- | --- | --- | --- |
| `runVoltage` | GPIO32 | V | Run-switch pin voltage |
| `directionVoltage` | GPIO33 | V | Direction-switch pin voltage |
| `potVoltage` | GPIO34 | V | Potentiometer wiper voltage |
| `potRaw` | GPIO34 | ADC counts, 0–4095 | Filtered reading used for Local frequency |

Voltages are sampled every 10 ms while the main loop is serviced. The browser requests status every second. The voltage fields are calibrated ADC estimates; they are not filtered in the same way as `potRaw`.

At 11 dB attenuation, the classic ESP32's documented measurable range is approximately **0.15–3.1 V**. Readings near either rail can saturate, so a pin tied to 3.3 V need not display exactly 3.3 V. See [Espressif's ADC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html).

## Connection detection

**Automatic connection detection and automatic source takeover are not implemented.** The interface currently reports voltage and switch state; it does not claim to know whether a control is physically plugged in.

With the current wiring:

- An open switch and an unplugged switch both leave the input pulled HIGH.
- GPIO34 has no internal pull-up or pull-down, so an unplugged potentiometer input can float.
- A voltage outside a chosen range cannot be interpreted reliably until the input circuit and valid ranges are defined.

Reliable detection requires distinct electrical states for connected OFF, connected ON and disconnected. A resistor network at the switch/controller or a separate presence contact can provide those states. A potentiometer likewise needs defined biasing or presence sensing. The circuit, thresholds, and whether takeover applies per input or to all controls still need to be specified.

GPIO34–39 restrictions are documented in [Espressif's GPIO reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html).

## Stop and direction behavior

Changing Local/Remote source disconnects all six motor outputs. In Local mode, the controller must observe an OFF switch position before an ON edge can start the outputs. The Local arming flag is visible in status.

Changing direction while running stops the motor outputs and requires OFF → ON before restarting. The new direction reverses the phase sequence; it does not measure rotor motion or wait for a measured standstill.

A browser/serial Stop prevents a held ON switch from restarting the outputs. Zero frequency or zero modulation also disables the motor outputs. GPIO25 is independent of motor Stop.
