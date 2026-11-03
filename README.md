Unofficial ESPHome-based local control for your Emporia Vue energy monitor.

- **Rapid response**: Data updates every five seconds. This allows for quick-responding automations and rapid feedback when your consumption changes.
- **Totally offline**: Everything is completely local, and nothing leaves your network. No internet? No problem, everything still works fine.
- **You're in control**: All the source code and configuration is available for you to use and modify to fit your particular situation. Data feeds into Home Assistant, where it can be visualized and acted upon.

See [the new website for installation instructions](https://emporia-vue-local.github.io/docs/tutorial/intro). Please leave any feedback or questions in the discussion section.

The older README that was previously here [is still available](https://github.com/emporia-vue-local/esphome/blob/dev/README.old.md).

## This fork: cycle capture (inrush / motor start meter)

With the cycle-capture SAMD09 firmware ([gekkehenkie11/emporia-samd09](https://github.com/gekkehenkie11/emporia-samd09), branch `cycle-capture`) the component can capture motor starts cycle by cycle: inrush current, start duration, voltage sag.

- **Build one:** [INRUSH_METER.md](INRUSH_METER.md), step by step from a stock Vue 2 to an inrush meter that logs every start in Home Assistant.
- **Firmware:** prebuilt SAMD09 images in [`samd09/`](samd09).
- **Example configurations:** [`examples/`](examples).
- **How the measurement works:** [CYCLE_CAPTURE.md](CYCLE_CAPTURE.md).
