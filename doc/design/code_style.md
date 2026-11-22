# Code style

Applies to `software/firmware/src` and `tests`.

## Layout

- Include guards `GTFOG_<PATH>_HPP`, never `#pragma once` (`GTFOG_APP_HPP`, `GTFOG_LOGIC_BATTERY_HPP`).
- `clang-format -i` with `.clang-format` at the repo root: LLVM base, 150 columns, no space before parentheses
  (`if(x)`), qualifiers to the right (`std::uint8_t const`). CI checks it.
- Include order: the module's own or local headers, `<zephyr/...>` and other C headers, then the standard library.
- Sources are listed by hand in `src/CMakeLists.txt` and `src/logic/CMakeLists.txt`, no globs.

## Naming

- `snake_case` for everything: functions, variables, types, namespaces, enumerators, constants; template parameters
  `CamelCase`, macros `UPPER_CASE`.
- Everything of ours is in `namespace gtfog`; board files keep their private names in an anonymous namespace.
- Whole words, no abbreviations, no throwaway names.
- Constants are `constexpr` in the file that uses them, with the unit in the type (`std::chrono` durations) or, for
  plain integers, in the name (`fan_min_duty_permille`, `cutoff_millivolts`).

## Comments

- At most two short sentences, describing the current code only: no history, no "previously".
- `ponytail:` marks a deliberate simplification or a calibration constant (what to measure or when to upgrade).
- No commented-out code, no unbuilt sources.

## Errors and preconditions

- No exceptions (`CONFIG_CPP_EXCEPTIONS` is not set): never `throw`.
- Board-layer functions that can fail return an `int`, negative errno on failure; callers check it, or ignore it with
  `std::ignore =` and a reason when the output is non-critical.
- `src/logic` has no failure paths; its functions are `noexcept` where they cannot fail.

## Zephyr specifics

- Devices, GPIOs, PWM and ADC channels come from devicetree specs (`GPIO_DT_SPEC_GET`, `PWM_DT_SPEC_GET`,
  `ADC_DT_SPEC_GET_BY_NAME`), never from port and pin numbers in code.
- Threads are `K_THREAD_DEFINE`; timed work is `K_WORK_DELAYABLE_DEFINE`; init hooks are `SYS_INIT`.
- Shared state is touched only through `with_controller` or under a Zephyr mutex or semaphore.
- Logging: `LOG_MODULE_REGISTER(name, LOG_LEVEL_INF)` once per board file (the application builds with `CONFIG_LOG=n`,
  the calls stay); `printk` is not used. `src/logic` does not
  log.
