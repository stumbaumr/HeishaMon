# Compressor short-cycle guard

A small, self-contained HeishaMon ruleset (`rules.txt`) that discourages the compressor from restarting immediately after it stops, in order to reduce short-cycling and its wear on the compressor. It's a good minimal example of the timer-armed-suppression pattern and of using a sentinel value on an existing topic instead of a dedicated `#` global to track state.

For the general rules-engine language (sigils, event blocks, built-in functions, device-side validation/safety behavior) see the `heishamon-rules` skill (`Examples/Rules/SKILL.md`).

## What the rules do

1. **Boot (`System#Boot`)** — arms `timer=1` with a 0 s delay, so the first thing that happens after boot (or after a rules reload) is `timer=1` firing and resetting `@SetZ1HeatRequestTemperature` to `0`. This guarantees the ruleset starts from a known, non-suppressed state even if it's reloaded while a suppression window from a previous run was still logically "active".

2. **Suppress on compressor stop (`on @Compressor_Freq`)** — fires on every change of `@Compressor_Freq`. When the compressor has just stopped (`@Compressor_Freq == 0`), and none of the following disqualify it:
   - `@Defrosting_State == 0` — don't interfere while a defrost cycle is in progress,
   - `@Outside_Temp >= 2` — only in mild weather, where a longer pause before restart carries no comfort or frost risk,
   - `@Z1_Heat_Request_Temp == 0` — only if there isn't already a heat request offset in play (also doubles as a re-entry guard, see below),

   then it drops `@SetZ1HeatRequestTemperature` to `-5` and arms `timer=1` for `2700` s (45 min). Lowering the requested zone-1 temperature offset makes the heat curve less eager to call for heat again, keeping the compressor off longer than it otherwise would be.

3. **End of suppression (`on timer=1`)** — after 45 minutes, restores `@SetZ1HeatRequestTemperature` to `0`, handing heat-request control back to the normal curve logic.

4. **Cold-weather override (`on @Outside_Temp`)** — if the outside temperature drops below `2 °C` while a suppression window is active (`@Z1_Heat_Request_Temp == -5`), immediately cancel the pending `timer=1` (`setTimer(1, 0)` re-arms it for 0 s, firing on the next tick) and reset the heat request to `0` right away. Cold weather takes priority over cycle-reduction — the unit should not be kept from calling for heat once conditions turn cold.

## Design notes for extending this pattern

- **`@Z1_Heat_Request_Temp == 0` / `== -5` as an implicit state flag.** Rather than adding a `#` persistent global just to track "is suppression active", this script reuses the read-back of the topic it already writes. That keeps the ruleset smaller (relevant given the ~10 KB best-practice limit) but only works because `-5` is otherwise not a value normal curve logic would produce for this topic — if you reuse this pattern elsewhere, pick a sentinel value that can't collide with a legitimate one.
- **Timer ID `1`** is the only timer this script uses. If you merge this into a larger ruleset (e.g. alongside `Jeisha-DHW-Radiators-Rowbuffer`), pick an unused timer ID — don't reuse `1` for something else.
- **Re-arming a timer to cancel it.** `setTimer(1, 0)` in the `@Outside_Temp` handler is the idiom for "fire (essentially) immediately instead of waiting out the original delay" — there's no separate "cancel timer" call in this engine.
- **Async write caveat still applies.** `@SetZ1HeatRequestTemperature = -5;` does not make `@Z1_Heat_Request_Temp` read back as `-5` within the same firing — the guard checks in later, independent event firings (`on timer=1`, `on @Outside_Temp`), not immediately after the write, so this script does not fall into that trap.
