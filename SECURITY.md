# Security Policy

## Supported Versions

This repository is currently maintained as a single rolling code line.

| Version / Branch | Supported |
| --- | --- |
| `main` | Yes |
| tagged releases | Best effort only |
| older commits and unpublished branches | No |

Security fixes are applied to the latest code on `main`.
There is currently no separate long-term support branch.

## Reporting a Vulnerability

Please do not report security vulnerabilities through public GitHub issues, pull requests, or discussions.

Use one of these private paths instead:

1. If GitHub private vulnerability reporting is enabled for this repository, use the `Security` tab and the `Report a vulnerability` flow.
2. Otherwise, contact the maintainer privately via GitHub: <https://github.com/kaspizzo>

When reporting a vulnerability, please include:

- a short summary of the issue
- the affected component or file
- steps to reproduce, if available
- impact and any suggested mitigation

I will try to acknowledge valid reports promptly and coordinate a fix before public disclosure.

## Current Security Model

This repository currently ships a community firmware setup flow, not a hardened
consumer appliance security model.

The current design assumptions are:

- the setup AP and captive portal are local onboarding/recovery paths
- the home-network portal is intended for a trusted LAN, or for a LAN where the
  user has explicitly enabled the portal admin password
- the controller stores Wi-Fi credentials, cloud credentials, machine binding
  data, and related tokens locally so it can operate without a companion app
- some controller features still depend on La Marzocco cloud reachability and
  backend behaviour

Operationally relevant limitations today:

- there is no OTA update path yet; firmware updates require a local USB flashing
  workflow
- the captive DNS implementation is intentionally minimal and only aims to make
  common captive portal flows work, with manual fallback to `http://192.168.4.1/`
- if the LAN portal is intentionally left open, anyone on that same LAN can use
  it; this is a deliberate trust tradeoff, not a hardened default

## Out Of Scope Threats

The current firmware does not claim to defend against:

- a hostile actor who already has access to the controller's setup AP during
  onboarding
- a hostile actor on the same home LAN when the user has deliberately left the
  portal open without an admin password
- physical possession, hardware probing, or debugger-level access to the device
- seamless remote security patch rollout without user-managed reflashing
