# .wasm loader reproducers

Modules that read past their own bytes in the interpreter's loader before the
fix beside them, each a libFuzzer finding. `custom_name_wraps.wasm` is a
custom section whose name length, added to its position, wraps a 32-bit
pointer: the boards' interpreter (`main.wasm`) is 32-bit.

Moybyte replays them with `tools/wasm_fuzz.py replay interp_classic32 FILE...
--wamr <this clone>`.
