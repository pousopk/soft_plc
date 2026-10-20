# Lesson 10: Integer and bit-string types

Milestone 10 adds the types nearly every Siemens program uses: the small, unsigned and 64-bit integers (`SINT`, `USINT`, `UINT`, `UDINT`, `LINT`) and the **bit strings** `BYTE`, `WORD` and `DWORD`. Bit strings come with bitwise `AND`/`OR`/`XOR`/`NOT`, the shifts `SHL`, `SHR`, `ROL` and `ROR`, access to single bits, bytes and words (`myWord.%X3`), typed literals (`WORD#16#FF00`), and a conversion between every pair of them.

```
status : WORD;                     a row of 16 bits, not a number
status := inputs AND 16#00F0;      bit by bit
door := status.%X3;                one bit, counted from the least significant
value := WORD_TO_INT(status);      to do arithmetic: say so, the bits are kept
count : UINT := UINT#65535;        a number that is never negative; +1 gives 0
```

Try it:
```bash
./build/tests/soft_plc_tests -tc="*bit*,*wrap*"   # the new types, on the bytecode engine
./build/st_parse -e "x := w.%X3 AND NOT b;"         # how a part access is parsed
```

---

## Part 1: PLC concepts

### 1.1 Bits are not numbers
A WORD read from `%IW0` holds 16 sensors; a WORD from a drive holds 16 status flags. Adding 1 to it means nothing, and comparing it with `-1` even less. IEC 61131-3 makes this a type difference: integers (ANY_INT) are numbers, bit strings (ANY_BIT) are patterns. You can combine bit strings bit by bit, shift them and test single bits, but not add them. TIA Portal follows the same rule when a block has *IEC check* on, and soft_plc always does: `myInt := myWord` needs `WORD_TO_INT`, which keeps the bits (`16#FFFF` becomes −1).

### 1.2 Unsigned types and wrapping
`USINT`, `UINT` and `UDINT` hold the same bits as `SINT`, `INT` and `DINT`, read as numbers that are never negative. They wrap like the signed types do: a UINT at 0 minus 1 is 65535. Automatic conversions only happen when every value fits, which is why INT and UINT don't convert into each other on their own, and why `anInt + aUint` is computed as a DINT, the smallest type that holds both.

### 1.3 Bit numbering and byte order
Siemens numbers bits from the least significant: `myWord.%X0` is the bit worth 1. But S7 memory stores a WORD **most significant byte first** (big-endian), so bit 0 of `%MW10` is physically in byte **11**. `%MX10.0` and `%MW10.%X0` are different bits. The guide shows the layout in chapter 8, and the test "Bit 0 of an AT word is the least significant bit, in the second byte" checks it.

### 1.4 Reading a REAL from two words
Fieldbuses like Modbus carry 16-bit registers, so a REAL arrives as two WORDs. Putting them together is a bit operation, and turning the 32 bits into a REAL must **copy the bits**, not convert the number: `DWORD_TO_REAL(16#3F80_0000)` is 1.0. That's why `DWORD_TO_REAL` and `REAL_TO_DWORD` are the only conversions that don't work on values.

---

## Part 2: Compiler concepts

### 2.1 One rule for widening
The analyzer decides every automatic conversion with one predicate, `widensTo(from, to)` in `src/st/analyzer.cpp`. For integers it compares ranges: a type widens to another when the other holds its whole range (`integerRange` in `src/st/ast.cpp`). So `USINT → INT` is allowed and `INT → UINT` isn't, without a table of pairs. `commonType(a, b)` then tries the candidates from smallest to largest and picks the first one both sides widen to.

### 2.2 Literals that adapt, and literals that don't
`16#FF00` is an *integer literal* (`DataType::AnyInt`): it takes the type of whatever it meets, if the value fits. `WORD#16#FF00` is a typed literal: the lexer turns `WORD#` into a `TypePrefix` token, the parser stores the type in the `Literal`, and from then on it behaves exactly like a WORD variable. That's why `SHL(16#81, 1)` is an error (an untyped literal isn't a bit string) but `SHL(BYTE#16#81, 1)` works.

### 2.3 Part access is read-modify-write
`w.%X3` isn't a place of its own: the analyzer turns it into the leaf `w` plus a `BitPart` (shift 3, width 1). Reading takes the whole value and extracts the bits; writing reads the whole value, replaces the bits and writes it back (`extractPart` and `insertPart` in `src/st/runtime_values.hpp`). Because it goes through `loadLeaf`/`storeLeaf`, AT variables get the right bits whatever their byte order, which simply adding 3 to a bit address would get wrong.

### 2.4 New instructions only where they're needed
Every new type is held in the same `int64_t` as INT and DINT, wrapped to its width after each operation. Unsigned values and bit strings are never negative once wrapped, so the existing compare, divide and `MOD` instructions already give the right answers. The VM only gets generic instructions for the new types (`WrapTo`, `StoreWrapped`, `AndBits`, ..., `LoadPart`, `StorePart`), so a program without them compiles to exactly the same code as before: the benchmark's disassembly didn't change by one instruction.

---

## Part 3: C++ concepts

### 3.1 Wrapping with casts
`wrapInteger` (`src/st/runtime_values.hpp`) wraps with a cast to the fixed-width type: `static_cast<std::uint8_t>(value)` for a USINT, `static_cast<std::int16_t>(value)` for an INT. Since C++20, converting to a signed type that can't hold the value is defined to wrap (it was implementation-defined before); converting to an unsigned type always wrapped.

### 3.2 `std::bit_cast`
`DWORD_TO_REAL` is `std::bit_cast<float>(static_cast<std::uint32_t>(bits))`: the same 32 bits, read as another type, with no undefined behaviour (unlike a `reinterpret_cast` or a union). The same function reads REALs from the process image.

### 3.3 Casting a double too big is undefined
`static_cast<std::int64_t>(1.0e30)` is undefined behaviour, not "the biggest value". `realToInteger` (`src/st/functions.cpp`) compares in `double` first and only casts values that fit. `INT64_MAX` as a double rounds up to 2^63, so "greater or equal" is the right test.

### 3.4 Shifts on unsigned numbers
Shifts and masks are done on `std::uint64_t`: shifting a negative signed number left is undefined before C++20, and shifting by the width or more is undefined in every version. `shiftBits` returns 0 for those before shifting.

---

## Part 4: Testing

- `tests/st/types_test.cpp` checks the names, widths, ranges and wrapping of every new type.
- `tests/st/analyzer_test.cpp` checks the IEC rules: no arithmetic on bit strings, no AND on integers, widening only where nothing is lost, the errors for typed literals and part access. `tests/st/arrays_structs_test.cpp` checks the S7 layout with byte-aligned 8-bit fields and a LINT in a data block.
- `tests/st/interpreter_test.cpp` and `tests/st/functions_test.cpp` run every new operation, conversion and shift, on both engines; two new differential tests run random inputs through the new types and through part accesses with run-time indices, comparing both engines after every cycle.
- `tests/monitor/monitor_test.cpp` checks reading and writing in hexadecimal, typed literals, and the whole LINT range.
- The guide's chapters 2, 3 and 6 have new programs and error examples, checked by `guide_test.cpp` (30 programs and 7 error examples in total).

---

## Review questions

**1.** Why does `w := w + 1;` fail when `w` is a WORD, and what do you write instead?
<details><summary>Answer</summary>

A WORD is a bit string, not a number, so `+` doesn't take it (IEC rule, like TIA Portal with IEC check on). Convert, add and convert back: `w := UINT_TO_WORD(WORD_TO_UINT(w) + 1);`. If you find yourself doing this often, the variable should probably be a UINT.
</details>

**2.** What is `INT + UINT`, and why isn't it INT or UINT?
<details><summary>Answer</summary>

A DINT. Neither INT nor UINT holds every value of the other (UINT has no negatives, INT stops at 32767), so both are brought to the smallest type that holds both, which `commonType` finds by trying the types from small to large.
</details>

**3.** `w AT %MW10 : WORD`. Which byte does `w.%X0 := TRUE;` change, and why?
<details><summary>Answer</summary>

Byte 11. `.%X0` is the least significant bit, and S7 memory stores a WORD with its most significant byte first, so the low byte is the second one. soft_plc gets this right because a part access reads and writes the whole WORD.
</details>

**4.** Why do `DWORD_TO_REAL(16#3F80_0000)` and `UDINT_TO_REAL(UDINT#16#3F80_0000)` give different results?
<details><summary>Answer</summary>

`DWORD_TO_REAL` copies the bits: `16#3F80_0000` is how a 32-bit float stores 1.0. `UDINT_TO_REAL` converts the number: 1 065 353 216 as a REAL.
</details>

**5.** Why don't unsigned types need their own compare and divide instructions in the VM?
<details><summary>Answer</summary>

Every value is kept in an `int64_t`, wrapped to its type after every operation. An unsigned value up to UDINT's 4 294 967 295 fits in an `int64_t` as a positive number, so a signed 64-bit compare or divide gives the same answer an unsigned one would. (ULINT wouldn't fit, which is one reason it isn't supported.)
</details>

**6.** Why is `SHL(16#81, 1)` an error, but `w := 16#81;` fine?
<details><summary>Answer</summary>

`16#81` is an untyped integer literal. In an assignment it takes the type of the target, WORD. SHL needs to know the width to shift in (a BYTE and a WORD give different results), and an untyped literal has none, so you must say it: `SHL(BYTE#16#81, 1)`.
</details>

**7.** Why does `realToInteger` compare with `>=` against the maximum instead of using `std::clamp`?
<details><summary>Answer</summary>

For LINT, the maximum (2^63 − 1) isn't representable as a double: it rounds to 2^63. Clamping to that and then casting to `int64_t` would be undefined behaviour. Comparing first and returning the integer maximum directly avoids the cast for every value that doesn't fit.
</details>

**8.** A program has `alarms : WORD` and you change it to `alarms : DWORD` with a download in RUN. What happens to its value?
<details><summary>Answer</summary>

It starts over at 0: online change keeps a variable only when its name **and type** are unchanged, and WORD and DWORD are different types. The LOAD reply lists it among the initialized variables.
</details>
