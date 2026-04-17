# Acknowledgements

This project is built on top of a lot of community work and reverse-engineering effort.

## Upstream software foundation

The software side of this repository builds primarily on:

- [`zweckj/pylamarzocco`](https://github.com/zweckj/pylamarzocco)

This repository extends that work into a standalone controller firmware and hardware-control project.

## Community research

The early community reverse-engineering work around La Marzocco connectivity helped establish a practical baseline for this project, especially the investigation shared by Plonx and other contributors in the public thread:

- <https://community.home-assistant.io/t/la-marzocco-gs-3-linea-mini-support/203581>

## Protocol and behaviour reference

`pylamarzocco` remains the main behavioural reference for cloud and BLE interactions in this space, and this repository benefits from that work directly.

## Controller hardware bring-up

The ESP32 controller firmware in this fork was informed by vendor hardware bundles, schematics, and example code for the JC3636K718-style round controller platform.

Those materials were useful for:

- display bring-up
- touch registration
- ring input bring-up
- haptic driver integration
- general board-level orientation

## Thanks

If this repository is published, this file should stay with it. The right public shape for a project like this is not "built from scratch", but "built carefully on top of community work that made it possible".

The raw JC3636K718 vendor package itself is intentionally not redistributed here. The public repository only keeps the cleaned project code and documentation needed to continue the controller firmware work.
