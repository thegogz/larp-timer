# LARP Timer

A Pebble smartwatch app for tracking multiple simultaneous cooldown timers during live-action roleplay (LARP). Designed to let you manage spell durations, ability cooldowns, and in-game timing without breaking immersion — no need to look at your watch to know which timer fired.

> **Note for contributors:** Update this README whenever you make changes that affect screenshots, features, or usage. Outdated docs are worse than no docs.

---

## Screenshots

| Default Screen | Timers Running | Flash Alert | Wrist-Flick Tap |
|:--------------:|:--------------:|:-----------:|:---------------:|
| ![Main](screenshot_main.png) | ![Running](screenshot_running.png) | ![Flash](screenshot_flash.png) | ![Tap](screenshot_tap.png) |

---

## Current State

Fully functional watchapp targeting the **Pebble Time 2 (Emery)** platform, built with the Pebble C SDK 4.17.

### Features

**5 independent timers**, each configurable with:
- **Custom name** — choose from 20 preset LARP-friendly labels (Spell, Potion, Shield, Stun, Heal, Ward, Rage, Curse, Trap, Move, Burn, Freeze, Drain, Boost, Timer, T1–T5)
- **Interval type:** Hourly, every X minutes (1–120), or every X seconds (1–59)
- **Vibration pattern:** 6 distinct patterns so you can identify which timer fired by feel alone

| Pattern | Feel |
|---------|------|
| Single `·` | One short pulse |
| Double `··` | Two short pulses |
| Triple `···` | Three short pulses |
| Long `—` | One long buzz |
| Long-Short `—·` | Long then short |
| Short-Long-Short `·—·` | Short, long, short |

**When a timer fires:**
- Vibrates with its unique pattern
- Full-screen **colour-coded flash** for 2 seconds — each timer has its own colour (red/blue/green/yellow/magenta) with the timer name and interval displayed large

**Wrist-flick tap interface (no-look operation):**

Start or stop a timer without looking at your watch:
1. Flick your wrist → arms the selector, yellow **TAP: -** bar appears at screen bottom
2. Each additional flick within 2 seconds increments the target: **TAP: T1**, **TAP: T2**, etc.
3. After 2 s with no further flick → automatically starts or stops that timer

This uses the accelerometer shock sensor, not a touchscreen — it works mid-combat with gloves on.

**Button navigation:**
- **UP / DOWN** — scroll through the timer list
- **SELECT** — action menu: Start/Stop or Configure
- **Long-press SELECT** — instantly toggles the highlighted timer (skips the action menu)
- **BACK** — exit / dismiss flash overlay

**Configuration menu** (SELECT → Configure, or tap an unconfigured timer):

Opens a persistent edit menu with three independently editable fields:
1. **Name** — pick from the preset name list
2. **Time** — choose interval type, then enter value via scroll wheel
3. **Vibration** — pick pattern (pre-selects your current setting)

Each field saves immediately on confirmation and returns to the edit menu, so you can change just the name or just the vibration without re-entering the other settings.

Timer configs and last-selected row persist across app launches. Timer running state resets on exit (by design).

---

## Install

### Emulator
```bash
pebble install --emulator emery
```

### Real Pebble (via Cloudpebble or phone)
```bash
pebble install --cloudpebble
```

Or sideload the PBW directly from `build/larp-timer.pbw` via the Pebble app.

### Build from source
```bash
pebble build
```
Requires [Pebble SDK 4.x](https://developer.rebble.io/developer.pebble.com/sdk/index.html).

---

## Roadmap

Planned additions:
- **More timers** — expand beyond 5 (dynamic add/delete, up to available heap)
- **Start All / Stop All** — single action to launch or halt the full set
- **Countdown warnings** — secondary vibration at a configurable threshold before a timer fires (e.g. buzz 30 s before the effect ends)
- **Per-timer colour customisation** — choose from the Pebble palette rather than using fixed defaults
- **Session profiles** — save and restore named sets of timer configurations for different game systems or character builds
- **Pebble app store release** — publish once the above QoL features are in place

---

## Project Structure

```
larp-timer/
├── src/c/main.c        # Full app source (~960 lines, single file)
├── package.json        # App manifest (UUID, display name, platform)
├── wscript             # Waf build script
├── build/
│   └── larp-timer.pbw  # Compiled watchapp bundle
└── screenshot_*.png    # Emulator screenshots (Emery platform)
```

---

## Platform

Targets **Emery (Pebble Time 2, 200×228 colour rectangular)**. The build system and C code are portable to other Pebble platforms (basalt, chalk, diorite) with minor geometry adjustments — colour features degrade gracefully on B&W platforms via `PBL_IF_COLOR_ELSE`.
