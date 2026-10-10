# Cycle capture: start / inrush measurements

With the **cycle-capture SAMD09 firmware** (`gekkehenkie11/emporia-samd09`, branch `cycle-capture`) the
Vue delivers one record per mains cycle: the RMS of the 3 voltages and 3 mains currents over that cycle, the
peak sample of the mains currents and two selectable 50A CT inputs. This component reads those records,
detects motor starts and reports for every start:

- the highest one-cycle RMS current
- the RMS over the first 100 ms (the same window a Fluke clamp uses for "inrush"; ±1 cycle)
- the highest instantaneous current
- the start duration (until the current stays below `settle_factor` x running current)
- the voltage before the start, the lowest one-cycle voltage and the sag in %
- the frequency range during the start
- whether a CT reached the ADC limit (then the peak values are too low)

The regular sensors keep working exactly as before. Without the new SAMD09 firmware the component logs an
error once and disables only the cycle capture.

## Example

```yaml
sensor:
  - platform: emporia_vue
    i2c_id: i2c_a
    phases:
      - id: phase_a
        input: BLACK
        calibration: 0.022
      - id: phase_b
        input: RED
        calibration: 0.022
      - id: phase_c
        input: BLUE
        calibration: 0.022
    ct_clamps:
      - { phase_id: phase_a, input: "A", current: { name: "Compressor L1" } }
      - { phase_id: phase_b, input: "B", current: { name: "Compressor L2" } }
      - { phase_id: phase_c, input: "C", current: { name: "Compressor L3" } }
    cycle_capture:
      trigger_current: 100            # A, any mains phase; a start is captured when it rises above this
      mains_current_multiplier: 3.0   # 600A CTs on the mains inputs (stock CTs: 1.0)
      aux_inputs: [1, 2]              # optional: two 50A CT inputs recorded per cycle as well
      max_current:                    # live: highest one-cycle RMS since the last publish (every live_interval)
        name: "Compressor max cycle current"
      min_voltage:
        name: "Compressor min cycle voltage"
      start_peak_current:
        name: "Start peak current"
      start_inrush_100ms:
        name: "Start inrush 100 ms"
      start_peak_instant:
        name: "Start peak instant"
      start_running_current:
        name: "Start running current"
      start_duration:
        name: "Start duration"
      start_voltage_min:
        name: "Start voltage min"
      start_voltage_sag:
        name: "Start voltage sag"
      start_clipped:
        name: "Start clipped"
      start_count:
        name: "Start count"
      on_start_captured:
        - homeassistant.event:
            event: esphome.compressor_start
            data:
              device: compressor-2
              summary: !lambda 'return summary;'
```

`summary` is a JSON string, `trace` a CSV string with one line per cycle (about 25 kB for a 5 s trace).
The summary fits easily in a Home Assistant event. For the full trace use MQTT, for example:

```yaml
      on_start_captured:
        - mqtt.publish:
            topic: compressor-2/start_trace
            payload: !lambda 'return trace;'
```

With `log_trace: true` (default) the trace is also written to the log, 4 lines per loop so the loop never
blocks. Copy it from `esphome logs` into a spreadsheet.

Example log of a start (simulated: grid, 600A CTs, locked-rotor current 444 A):

```
Start 1: peak 442 A (one cycle), inrush 100 ms 266 A, instant 628 A, running 50.0 A, duration 0.98 s,
         Vmin 116.7 V (sag 8.1%), f 59.85-60.12 Hz, clipped: -, gaps 0
start 1 trace: t_ms,i_A,i_B,i_C,pk_A,pk_B,pk_C,v_BLACK,v_RED,v_BLUE,hz,clip,flags,aux0,aux1
start 1 trace: 0,266.4,266.4,266.5,378,377,378,121.4,121.4,121.4,59.99,0,0,4.09,82.43
start 1 trace: 200,441.1,441.9,442.3,627,628,627,116.7,117.0,117.0,59.85,0,0,4.12,87.76
...
```

## Options

| Option | Default | |
|---|---|---|
| `trigger_current` | 100 | A. Capture when any mains phase rises above this |
| `rearm_current` | trigger / 2 | A. All phases must be below this for 0.2 s before the next capture. The start onset (t = 0) is the first cycle above this level, so a part-winding first step or a current-limited start below the trigger is included |
| `pre_trigger` | 500ms | kept before the onset |
| `post_trigger` | 5s | recorded after the trigger |
| `settle_factor` | 1.3 | the start ends when the current stays below this x the running current (RMS of the last 0.5 s) |
| `mains_current_multiplier` | 1.0 | for mains CTs with another rating than the stock ones (600A: 3.0, if its output at 600 A equals the stock CT at 200 A — check with a clamp meter) |
| `aux_inputs` | none | up to two 50A CT inputs (1..16) recorded per cycle |
| `aux_current_multiplier` | 1.0 | |
| `poll_interval` | 50ms | how often the ring buffer is read (max 400ms; the ring holds ~0.5 s) |
| `live_interval` | 1s | publish interval of the live sensors |
| `log_trace` | true | write the trace to the log |
| `frame_rate` | measured | SAMD09 ADC frames per second. Leave out: it is measured against the ESP32 clock over 60 s windows |

Live sensors: `max_current`, `min_voltage`, `min_frequency`, `max_frequency`.
Per start: `start_peak_current`, `start_inrush_100ms`, `start_peak_instant`, `start_running_current`,
`start_duration`, `start_voltage_min`, `start_voltage_sag`, `start_clipped` (0/1), `start_count`.
Per start values are the maximum over the phases (voltage: minimum over the wires); the JSON has them per phase.

## Reading the results

- **clipped**: the CT/ADC reached its limit. With the stock 200A CTs that happens at about 263 A RMS
  (372 A peak). Peak and inrush values are then lower than reality. Use higher rated CTs.
- **gaps**: records were lost (the ESP did not read in time). **merged**: the SAMD09 main loop was late and a
  record covers two cycles. Both should be 0.
- **settled: false**: the current was still above the settle level at the end of the trace (increase
  `post_trigger`, or the motor did not come up to speed).
- **t = 0** is the first cycle above `rearm_current`. The start can begin mid-cycle, so the first cycle may be
  lower.

## Checking against a Fluke

Put the Fluke (inrush mode) and the Emporia CT on the same conductor and compare the Fluke value with
`inrush_100ms_a` of the same start. They should agree within a few percent; then the rest of the trace is right
as well.

## Implementation

- `cycle_capture.h/.cpp`: protocol, record checks and the start analysis. No ESPHome dependencies, unit tested
  on a PC together with the real SAMD09 firmware (see `test/` in the SAMD09 repository).
- The analyzer keeps raw 32-byte records (a 5 s trace is ~11 kB) and generates trace lines on demand.
