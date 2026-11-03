# SAMD09 firmware for the Emporia Vue 2

Prebuilt images of the open-source SAMD09 firmware from
[gekkehenkie11/emporia-samd09](https://github.com/gekkehenkie11/emporia-samd09).
The SAMD09 is the chip on the Vue 2 that samples the voltages and currents; the ESP32 reads it over I²C.

| File | Source | What it does |
|---|---|---|
| `EmporiaSamd09-cycle-capture-v2.bin` / `.hex` | branch `cycle-capture`, commit [`14e9495`](https://github.com/gekkehenkie11/emporia-samd09/commit/14e9495) | the normal 0.5 s packet (byte-identical) **plus** one record per mains cycle for start/inrush measurements, and interrupt timing in the status (status version 2) |
| `EmporiaSamd09-standard.bin` / `.hex` | branch `main`, commit [`c9cc36a`](https://github.com/gekkehenkie11/emporia-samd09/commit/c9cc36a) | open-source replacement that does the same as the factory firmware, without cycle capture. Use it to go back |

Both work with the regular ESPHome `emporia_vue` component. Cycle capture only works with the component in this
repository (branch `cycle-capture`).

Check the download with `SHA256SUMS`:

    sha256sum -c SHA256SUMS

The images start at address `0x00000000` (no bootloader). Flashing is described in
[INRUSH_METER.md](../INRUSH_METER.md#step-2-flash-the-samd09).

## Build it yourself

    git clone -b cycle-capture https://github.com/gekkehenkie11/emporia-samd09.git
    cd emporia-samd09
    make

Needs `arm-none-eabi-gcc` (tested with GCC 13). The result is `build/EmporiaSamd09.bin` (14632 bytes text) and
should have the same SHA256 as the file here. The `test/` folder runs the real `main.c` on a PC against the
ESPHome analyzer (`make legacy`, `make sim`).
