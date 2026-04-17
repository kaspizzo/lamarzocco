# Publishing Checklist

This file is the short list to walk through before making the repository public.

## Already prepared

- Root `README.md` now describes the repository as a controller-focused project
- public attribution text exists in `ACKNOWLEDGEMENTS.md`
- firmware README has been updated to the current UI/setup flow
- controller setup and flashing docs already exist under `docs/controller/`

## Still recommended before pressing "Public"

### 1. Keep the repository licensing boundary clear

- The repository source code and docs now ship under Apache-2.0
- Keep `LICENSE` and `NOTICE` at the repository root
- Keep the raw `JC3636K718_knob_EN/` vendor bundle out of the public repository
- Keep official vendor logo files out of the shipped defaults and out of the repository tree
- Still review any copied screenshots, binaries, PDFs, or vendor snippets separately before publishing them

### 2. Review attribution one more time

- Keep `ACKNOWLEDGEMENTS.md`
- Keep `NOTICE`
- Keep upstream links in the root README
- Make sure any release notes also mention that this builds on `zweckj/pylamarzocco`

### 3. Scrub local/private artifacts

Before public release, verify that the repository does not expose:

- personal screenshots
- machine serial numbers from logs
- local network names
- cloud credentials
- BLE tokens
- local hostnames or machine-specific debug dumps that should not be public
- official La Marzocco logo assets copied from apps, websites, or firmware bundles

Also review embedded credentials or installation keys used by the firmware itself.
If the controller still depends on hardcoded cloud-installation secrets or private keys, treat that as a separate publication blocker even if the values are not tied to your personal machine.

### 4. Decide what should stay internal

This repository currently contains developer debug tooling and bring-up notes.

Before release, decide whether:

- the PoC web tools should stay in the main repo as supported developer utilities
- all controller docs should stay public as-is
- any hardware notes are better moved into a separate `notes/` or `internal/` area before publishing
- any screenshots or README imagery still imply official branding instead of plain-text compatibility

### 5. Final metadata pass

Before the repo goes public, verify:

- GitHub repo description
- topics/tags
- default branch protection
- issue tracker settings
- discussion settings, if wanted
- release visibility and first version tag

### 6. Cut a first public release cleanly

A good first public release should include:

- a short changelog
- supported hardware statement
- supported machine statement
- known limitations
- explicit "community project / not official" wording
- compatibility wording in text rather than official vendor logos
- a note that cloud onboarding currently expects a direct La Marzocco email/password account, not Apple/Google-only sign-in
- a short onboarding note that first boot and full factory reset reopen the setup AP with the QR code on-screen
- a brief gesture note for `swipe down = presets`, `swipe up = setup`, and `long-press in setup = network reset flow`

## Suggested first release notes outline

Use something like this:

1. What this repo contains
2. What is already working
3. What is still experimental
4. First-boot / factory-reset onboarding
5. Account requirements and known login limitations
6. Supported machine and firmware scope
7. Supported controller hardware
8. Touch gestures and reset behaviour
9. Credits and upstream references
