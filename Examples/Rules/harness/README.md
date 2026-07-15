# Host-side rules harness

Compiles the **real firmware rules engine** (`HeishaMon/src/rules/`) as a Linux binary and drives it against simulated heat-pump state, so a ruleset can be validated and behavior-tested without flashing a device. Rulesets normally have no lint/test tooling at all — validation happens only on-device at upload time; this harness runs the exact same `rule_initialize()` parse/validation on your PC and then goes further by actually executing scenarios.

## Build

```
./build.sh
```

Needs `g++` and `python3` (no Arduino toolchain). `build.sh` first regenerates `valid_names.h` from the firmware's topic/command tables (`decode.h`, `commands.h`), so `@Name` typos in a ruleset fail the parse just like real unknown names would.

## Usage

```
./harness <rules.txt> parse        # parse + on-device-style validation only
./harness <rules.txt> all          # run every simulation scenario
./harness <rules.txt> day|dhw|lowtank|throttle|pause|locals   # single scenario
```

Every `@Set…` command a ruleset emits is printed to stdout as a `CMD [t=<virtual-seconds> <hh:mm>] @SetX = <value>` line; scenario markers (`===`, `---`, `--`) and end-of-scenario persistent-global dumps (`VAR #… = …`) are printed alongside. Engine/parse diagnostics go to stderr.

The built-in scenarios are regression scenarios for the `Jeisha-DHW-Radiators-Rowbuffer` example: simulated days across the scheduled hours in heating/cooling weather, full DHW cycles including defrost-during-DHW and the recovery timer chain, the low-tank fallback, a throttle sweep across outlet/target deltas in heating, DHW and cooling modes, and the smart-pause path. For other rulesets, `parse` works as-is; add or adapt scenarios in `harness.cpp`.

## Verifying a refactor is behavior-preserving

```
./build.sh
./harness old-rules.txt all 2>/dev/null | grep '^CMD\|^===\|^---\|^--\|^VAR' > old.filtered
./harness new-rules.txt all 2>/dev/null | grep '^CMD\|^===\|^---\|^--\|^VAR' > new.filtered
diff old.filtered new.filtered && echo IDENTICAL
```

An empty diff means both rulesets emitted the same commands with the same values at the same virtual times, and ended with the same persistent-global state, across all scenarios. (The grep strips engine timing lines, which differ run to run.)

## How the simulation works

- Readable `@topics` come from an in-memory table the scenarios control; **`@Set…` writes are recorded, not fed back** — like the real device, where a command's effect only appears on the read topic later. Scenario scripts update read topics explicitly to simulate the heat pump responding.
- `%hour`/`%minute` are scenario-controlled, and timers run on a **virtual clock** (`advance(seconds)` elapses due timers in order), so a full simulated day takes milliseconds.
- Local/global `$`/`#` variable handling mirrors the device glue in `HeishaMon/rules.cpp`, including freeing local stacks after every firing.
- The `locals` scenario demonstrates where function-block `$` locals live after a firing (the block's own stack, not the trigger's).
