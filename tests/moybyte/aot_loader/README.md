# AOT loader reproducers

AOT module files for the ESP32-P4 (`riscv32/`) and the ESP32-S3 (`xtensa/`)
that crashed the loader, its relocations or a module's instantiation before
the fixes beside them, each a libFuzzer finding minimized. Every one must be
refused or loaded without touching memory outside what the module owns.

Moybyte replays them: `tools/wasm_fuzz.py replay aot_rv32|aot_xtensa FILE...
--wamr <this clone>` builds this tree's loader for the chip, 32-bit, under
AddressSanitizer and UndefinedBehaviorSanitizer, and runs each file through
the board's admission, the loader, the provenance check and instantiation;
`tests/test_wasm_hostile.py` there replays the copies it keeps.
