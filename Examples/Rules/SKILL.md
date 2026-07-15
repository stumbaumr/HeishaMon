---
name: heishamon-rules
description: How to read and write HeishaMon rules-engine scripts (rules.txt) — the sigils (@/#/$/%), event/function blocks, built-in functions, and the device-side validation/safety behavior. Use before writing or editing any HeishaMon ruleset, including the worked examples under Examples/Rules.
---

A HeishaMon ruleset is a small event-driven DSL, parsed and run by the firmware's own interpreter (`HeishaMon/src/rules/rules.cpp` in this repo). There is no build/lint step for a ruleset itself — it's plain text uploaded through the device's web UI (`/rules`) or REST API. For off-device checking there is a host harness in `Examples/Rules/harness/`: `./build.sh && ./harness <rules.txt> parse` runs the same parse/validation the device performs at upload (including `@Name` checks against the real topic/command tables), and its scenario mode records emitted `@Set…` commands so two ruleset versions can be diffed for behavioral equivalence — see its README. Use it before uploading any non-trivial ruleset edit. The canonical, most up-to-date grammar reference lives at https://github.com/CurlyMoo/rules — check there if something below seems out of date.

## Coding Standards

### Code Style for HeishaMon Rules
- **3-space indentation** (not tabs)
- **Single quotes** for strings
- **LF line endings**
- **UTF-8 charset**

## Sigils (variable lifetime and source)

- `@Name` — an **MQTT topic** HeishaMon exposes. Read-only sensor topics (e.g. `@Outside_Temp`, `@Heatpump_State`, `@Operating_Mode_State`) reflect live heat pump state. Write-capable `@Set…` topics (e.g. `@SetHeatpump`, `@SetOperationMode`, `@SetDHWTemp`, `@SetCurves`) publish a command to the heat pump when assigned to. See `MQTT-Topics.md` / `Manage-Topics.md` in the repo root for the full topic list, and note the R/W split: reading a value and setting it go through different topic names.
- `#name` — a **persistent global**. Survives across rule firings but *not* across a reboot — re-seed these in `on System#Boot`. Names are case-sensitive (`#opMode` and `#OpMode` are different variables); pick a casing convention and stay consistent within a script.
- `$name` — a **local**, scoped to a single `on … end` block.
- `%name` — an **engine-supplied datetime variable**: `%hour` (0–23), `%minute` (0–59), `%month` (1–12), `%day` (1–7). Requires NTP to be configured correctly.
- `?name` — a **thermostat parameter** (OpenTherm), when OpenTherm support is enabled. Same names for read and write; not all are readable/writable — check the device's OpenTherm tab.
- `ds18b20#<sensor-id>` — a Dallas 1-wire temperature reading, read-only.
- `s0#watt[hour[total]]_<port>` — S0 kWh-meter values for port 1 or 2, e.g. `s0#watt_1`, `s0#watthourtotal_2`.

A variable read before it's ever set evaluates to `NULL`. Types are boolean (`1`/`0`), integer, float, or string (single- or double-quoted).

## Blocks

```
on [event] then
  ...
end
```

- `on System#Boot then … end` — runs once at firmware start and again after a new ruleset is saved. The natural place to seed `#…` defaults and arm initial timers.
- `on @<Topic> then … end` — fires whenever that MQTT topic's value changes.
- `on timer=<id> then … end` — fires when a timer armed by `setTimer(<id>, <seconds>)` elapses. Timers are one-shot; re-arm inside the handler for a recurring tick. **Timer IDs are just integers you pick — don't reuse one for two unrelated purposes within a script**, or the handlers will collide.
- `on ?<param> then … end` — fires when a thermostat (OpenTherm) parameter changes.
- `on <name> then … end` plus `<name>(args…)` — a user-defined function/event, callable from anywhere else in the script. Missing call arguments become `NULL` inside the block.

## Operators

Standard precedence/associativity, parenthesization works as in normal math: `&& || == >= > < <= - % * / + ^`. `%` is integer modulo (don't confuse with the `%hour`-style sigil above — context disambiguates).

## Built-in functions

`coalesce(…)` (first non-`NULL` argument, unlimited args), `max(…)`, `min(…)`, `isset(x)` (true iff `x` is still `NULL`), `round(x)`, `floor(x)`, `ceil(x)`, `setTimer(id, seconds)`, `print(x)`, `concat(…)` (string concatenation — handy for building a JSON-ish payload for a topic like `@SetCurves`), `gpio(pin)` / `gpio(pin, state)` (read or set a digital GPIO; on the ESP32-S3 board the two relays are `gpio(21, …)` and `gpio(47, …)`).

## Device-side safety behavior (don't route around these)

- A newly-uploaded ruleset is **validated immediately**. If invalid, it's rejected and the previous ruleset keeps running — check the console/log for why.
- If a valid-looking ruleset **crashes the device**, HeishaMon auto-disables rules on the next boot so you can recover without a boot loop. Don't try to defeat this from within a script.
- Keep rulesets under roughly 10 KB (best practice from the firmware author, not a hard limit).
- Sending a command to the heat pump is **asynchronous** — a `@Set…` write earlier in a block is not reflected in the corresponding read-topic later in the *same* firing. Always branch on the heat pump's own reported state, not on a value you just wrote.
- The heat pump's serial link is slow. A single rule firing that sends a burst of `@Set…` commands can overrun HeishaMon's command throughput and drop some of them. Prefer spreading a burst of commands across a couple of staggered one-shot timers (see the worked example below for this pattern) over emitting many `@Set…` writes from one block.

## Worked examples

- `Examples/Rules/Jeisha-DHW-Radiators-Rowbuffer/` contains a real multi-year-tuned ruleset (`rules.txt`) plus `README.md`, a block-by-block walkthrough of *why* it's written the way it is — DHW/defrost state machine, timer-budget discipline, pre-DHW state snapshotting, etc. Read `README.md` alongside `rules.txt` when you need to see these primitives combined into a non-trivial controller, or when extending that specific ruleset.
- `Examples/Rules/Compressor-Short-Cycle-Guard/` is a small, focused ruleset that suppresses the zone-1 heat request for a fixed window after the compressor stops (mild weather only) to reduce short-cycling, with a cold-weather override. Good minimal example of the timer-armed-suppression pattern and of using a sentinel value on an existing topic instead of a `#` global to track state.
