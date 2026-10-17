# Lesson 8: Arrays and structs

Milestone 8 adds the two ways real PLC programs group data: **arrays** (`ARRAY[1..10] OF INT`) and **structs** (`TYPE Point : STRUCT x : REAL; y : REAL; END_STRUCT; END_TYPE`), the equivalent of UDTs in TIA Portal. They can be nested in any combination, copied as a whole, passed to function blocks, and placed in a data block with the same byte layout a Siemens CPU uses. The milestone also starts the [programming guide](../guide/README.md).

```
TYPE … END_TYPE ──► analyzer: one Type per type, with its leaves and S7 layout
line[i].start.x  ──► analyzer: variable + fixed offset + run-time index terms
                 ──► interpreter / VM: one "place" = instance + variable + offset
```

Try it:
```bash
./build/soft_plc examples/inspection.st                 # four cameras' results in DB10
./build/st_parse --bytecode examples/inspection.st      # see how the accesses compile
```

---

## Part 1: PLC concepts

### 1.1 Why PLC programs need arrays and structs
Without them, four cameras mean `cam1Ok`, `cam1Score`, `cam2Ok`, `cam2Score`… and four copies of the same logic. With a struct `Result` and `cameras : ARRAY[1..4] OF Result`, the logic is written once inside a `FOR cam := 1 TO 4` loop, and the interface to the vision system is one documented type. That's why almost every real project has UDTs for its interfaces: a drive, a recipe, a camera result, a robot handshake.

### 1.2 The S7 standard layout
**Code:** `Analyzer::makeStruct`, `Analyzer::makeArray` (`src/st/analyzer.cpp`)

A Siemens CPU with *standard* (non-optimized) access puts the fields of a struct at fixed byte offsets, following simple rules:
- a BOOL takes the next free bit, so consecutive BOOLs share a byte;
- INT, DINT, REAL and TIME start at the next **even** byte;
- a struct or array starts at an even byte and takes an even number of bytes;
- `ARRAY OF BOOL` packs its elements bit after bit.

So `Result` (`ok`, `nok`, `sequence : INT`, `score : REAL`, `posX`, `posY : REAL`) is `ok` at 0.0, `nok` at 0.1, a gap of one byte, `sequence` at 2, `score` at 4, and so on: 16 bytes. TIA Portal shows exactly these offsets in a non-optimized DB, and that's what an HMI or a vision system reading the DB with PUT/GET uses. soft_plc follows the same rules when an array or struct is placed with `AT`, and the test *A struct in a DB is where an S7 client expects it* checks it with a real Snap7 client.

**Optimized** blocks (the default in TIA for S7-1200/1500) have no fixed offsets: the CPU arranges the memory as it likes, and you can only reach variables by name. soft_plc's variables without `AT` are the same: no byte offsets, only names.

### 1.3 Consistency, and why the sequence number goes last
An S7 client only gets consistent data **within one request**. A large DB read in several requests can mix two PLC cycles. `inspection.st` writes each camera's result and **then** increments its `sequence`. A client that polls `sequence` and sees it change knows the rest of that result is complete, and a stale buffer can't be mistaken for a new result. That's the same idea as the handshake between a vision system and a PLC that ignores the result-ready signal.

### 1.4 Index errors
A constant index outside the bounds (`a[11]` for `ARRAY[1..10]`) is caught by the compiler. A computed one (`a[i]`) can only be checked while running, so it's checked **every time**, and an out-of-range index puts the PLC in STOP with the line and column. Writing past the end of an array silently would corrupt whatever lies next to it in memory, which is the worst kind of PLC bug to find.

---

## Part 2: Compiler concepts

### 2.1 Types as data
**Code:** `struct Type` (`src/st/ast.hpp`)

Until now a type was just an enum value (`DataType::Int`). An array or struct type has structure: its dimensions, its element type, its fields. So the analyzer builds a `Type` object for each one:

```cpp
struct Type {
    Kind kind;                              // Elementary, Array or Struct
    std::string name;                       // "Point", "ARRAY[1..3] OF INT"
    std::vector<ArrayDimension> dimensions; // Array
    const Type* element;                    // Array
    std::vector<Field> fields;              // Struct: name, type, leafOffset, bitOffset
    std::vector<Leaf> leaves;               // every elementary value inside, in order
    std::size_t bitSize;                    // size in the S7 layout
};
```

Types point at other types (`element`, `Field::type`), forming a graph that the analyzer builds once and everything else only reads.

### 2.2 Leaves: flattening data to make indexing arithmetic
**Code:** `Type::leaves`, `VariableDeclaration::leaf`

The interpreter's memory is still a flat `std::vector<Value>` per instance. An array or struct simply takes several consecutive entries, one per **leaf** (each BOOL, INT, DINT, REAL or TIME inside it):

```
VAR a : INT;  p : Point;  pts : ARRAY[1..3] OF Point; END_VAR

values:  [ a | p.x  p.y | pts[1].x  pts[1].y  pts[2].x  pts[2].y  pts[3].x  pts[3].y ]
leaf:      0    1    2      3         4         5         6         7         8
```

With this layout, every access is "first leaf of the variable + offset":
- a field adds a constant: `.y` is +1 inside a `Point`;
- an index adds `(i - low) × leaves per element`: `pts[i]` is `3 + (i - 1) × 2`.

For variables placed with `AT`, the same calculation is done in **bits** of the S7 layout instead of leaves; that's why every `Field` and `IndexTerm` has both a leaf and a bit stride.

### 2.3 Do it once: fixed offsets and run-time terms
**Code:** `Analyzer::resolveAccess`, `struct Access`

The analyzer walks `line[2].start.x` and adds up everything it already knows: the field offsets, and constant indices like `[2]`. Only indices that depend on variables are left for run time, as `IndexTerm`s (expression, bounds, stride). When nothing is left and the variable isn't `AT`, the final leaf goes into `directLeaf`, and both engines read it with a single array access. The bytecode shows the difference:

```
x := a[3];     LoadInt 2               ; a is leaves 0..9: a[3] is leaf 2, fixed
               StoreInt16 11
x := a[i];     LoadInt 10              ; push i
               Index 0                 ; check 1 <= i <= 10, push (i - 1) × 1
               LoadPlace 0             ; a + that offset
               StoreInt16 11
```

### 2.4 One "place" for both engines
**Code:** `Interpreter::place`, `loadLeaf`, `storeLeaf`, `copyAggregate`

A *place* is "this instance, this variable, this offset". Both the tree-walker and the VM's `LoadPlace`/`StorePlace`/`CopyPlace` instructions go through the same three functions to read, write and copy. So bounds checks, AT addressing and type conversions can't drift apart between the engines, and the differential test checks they don't.

### 2.5 Nominal and structural types
**Code:** `sameType()` (`src/st/ast.cpp`)

When can `a := b;` copy a whole value? Two structs must be the **same declared type**, even if another type has identical fields: a `Point` is not a `Size` just because both have two REALs. That's *nominal* typing, by name. Two arrays only need the same bounds and the same element type: *structural* typing. This matches what TIA Portal allows, and it's the most common split in programming languages.

### 2.6 Recursive types and cycles
**Code:** `Analyzer::resolveDeclaredType`

Types may be declared in any order, so `Line` can use `Point` declared below it. The analyzer resolves types on demand, remembering which ones it's in the middle of (`resolving_`). Meeting one of those again means a type contains itself (`A` contains `B` contains `A`), which would need infinite memory, so it's an error with the whole chain in the message.

---

## Part 3: C++ concepts

### 3.1 Owning objects that others point to
**Code:** `CompilationUnit::analyzedTypes`

```cpp
std::vector<std::unique_ptr<Type>> analyzedTypes;
```

Accesses, fields and variables hold `const Type*` pointers. A `std::vector<Type>` would move its elements when it grows, leaving those pointers dangling. A vector of `unique_ptr<Type>` only moves the *pointers*; each `Type` stays where it was allocated. It's the same reason the S7 server keeps `unique_ptr<SharedArea>` (lesson 6).

### 3.2 A pointer into a tree you own
**Code:** `IndexTerm::index`

An `IndexTerm` points at an index expression that's owned by the same `Access` (inside its `IndexSelector`). That's safe because the expression lives on the heap (`unique_ptr<Expression>`): moving the `Access`, or the whole tree, moves the `unique_ptr`, not the `Expression` it owns. `cloneAccess` copies only what the parser wrote and leaves the analyzer's pointers out, because copied pointers would point into the *original* tree.

### 3.3 Recursion that follows the type
**Code:** `Interpreter::initialize`

Initial values are applied by a function that calls itself following the type's shape: an elementary type sets one leaf; a struct calls itself for each field (with the given value or the field's default); an array calls itself for each element. Code that walks a recursive data structure usually looks like the data structure. `checkInitializer` in the analyzer has the same shape.

### 3.4 A shared instance built once
**Code:** `elementaryType()` (`src/st/ast.cpp`)

The five elementary types are needed everywhere (array elements, fields, variables) but never change, so there's one `Type` object for each, built on first use in a function-local `static` (lesson 4, section 3.4) and shared by every compilation.

### 3.5 Parsing paths without allocating: `std::from_chars`
**Code:** `resolvePath` (`src/st/interpreter.cpp`)

Monitoring paths like `cameras[2].score` are parsed with `std::string_view::find_first_of` and `std::from_chars`, which read the text in place without making temporary strings, and report exactly where a number ends. Unlike `std::stoi`, `from_chars` never throws, so a bad path like `a[x]` just doesn't resolve.

---

## Part 4: Testing

### 4.1 Tests for every rule
`tests/st/arrays_structs_test.cpp` has a table with one row per compile error (about 35), and tests that run programs on both engines: indexing, negative bounds, row-major order, nested structs, copies being independent, every kind of initial value, the run-time index error with its line and column, and function block interfaces.

### 4.2 The memory layout, checked from the outside
Two tests check the S7 layout: one looks at the bytes of the data block directly; the other runs `examples/inspection.st` with the S7 server and reads DB10 with a **Snap7 client**, the library `plc_gui` uses, comparing every field at its documented offset with what the program wrote.

### 4.3 Differential testing again
A new stress program uses arrays and structs in every way (in memory and with `AT`, run-time indices from random inputs, whole copies, a function block with an array input and a struct output) and runs 2000 random cycles on both engines, comparing every variable and every byte of memory after each cycle. A second test checks both engines stop with the same index error at the same place.

### 4.4 Documentation that's tested
`tests/st/guide_test.cpp` reads the programming guide, extracts every complete program and compiles and runs it, and checks that every example marked `(* error: … *)` fails with exactly that message. If the language changes and the guide isn't updated, the tests fail.

### 4.5 Measuring performance without fooling yourself
The refactor was checked with `st_bench`, and at first the tree-walker looked 40% slower. Two things were wrong with that measurement: the machine was busier than when the README's numbers were taken (the *old* code was also slower now), and the "new" binary turned out to be stale. Counting instructions with **callgrind** (`valgrind --tool=callgrind`), which gives the same number on every run, showed the real cost: about 4% more instructions in total, almost all in the tree-walker. Lesson: compare old and new **side by side, on the same machine, at the same time**, and prefer a deterministic measure when timings are noisy.

---

## Review questions

**1.** `VAR a : INT; p : Point; END_VAR` with `Point` = two REALs. Which leaf is `p.y`?
<details><summary>Answer</summary>

Leaf 2. `a` is leaf 0, `p` starts at leaf 1, and `y` is the second field of `Point`, at offset +1.
</details>

**2.** `pts : ARRAY[1..3] OF Point` starts at leaf 3. Which leaf is `pts[i].y`, as a formula?
<details><summary>Answer</summary>

`3 + (i - 1) × 2 + 1`. Each element has 2 leaves, the array starts at 1, and `y` is +1 inside the element. The `+ 1` and the start are fixed, so the analyzer adds them up front (offset 4); only `(i - 1) × 2` is computed at run time.
</details>

**3.** In the S7 layout, where are the fields of `STRUCT a : BOOL; b : INT; c : BOOL; END_STRUCT`, and how big is it?
<details><summary>Answer</summary>

`a` at 0.0; `b` at byte 2 (INT starts at the next even byte, so byte 1 is a gap); `c` at 4.0. The struct ends after byte 4, and its size is rounded up to an even number of bytes: 6 bytes.
</details>

**4.** Why does `inspection.st` increment `sequence` after writing the other fields, not before?
<details><summary>Answer</summary>

A client that reads in several requests (or at an awkward moment) might see some new and some old fields. With the sequence number written last, a client that waits for the sequence to change knows the result that goes with it is complete.
</details>

**5.** `a[11]` is a compile error, but `a[i]` with `i = 11` is a STOP. Why can't the compiler catch both?
<details><summary>Answer</summary>

The value of `i` is only known while the program runs (it may come from an input, a counter, an HMI). The compiler can only check constants; everything else has to be checked at run time, every time.
</details>

**6.** `TYPE Point : STRUCT x, y : REAL; END_STRUCT; Size : STRUCT x, y : REAL; END_STRUCT; END_TYPE`. Is `p := s;` allowed, with `p : Point` and `s : Size`? And `a := b;` with `a : ARRAY[1..2] OF REAL` and `b : ARRAY[1..2] OF REAL`?
<details><summary>Answer</summary>

`p := s` is an error: structs must be the same declared type (nominal typing), even with identical fields. `a := b` is fine: arrays only need the same bounds and element type (structural typing).
</details>

**7.** Why does `CompilationUnit` hold its types as `std::vector<std::unique_ptr<Type>>` and not `std::vector<Type>`?
<details><summary>Answer</summary>

Lots of code keeps `const Type*` pointers. A `vector<Type>` moves its elements to new memory when it grows, so those pointers would dangle. With `unique_ptr`, each `Type` is allocated once and never moves; only the small pointers inside the vector move.
</details>

**8.** Both engines call the same `place()`, `loadLeaf()` and `storeLeaf()`. What would go wrong if each engine had its own copy of that code?
<details><summary>Answer</summary>

Two copies drift apart: a bug fixed in one, an error message changed in one, an S7 layout detail different in one. The engines would then give different results for the same program, which is exactly what the differential tests exist to catch. Sharing the code makes the behaviour identical by construction.
</details>

**9.** A benchmark of new code looks 40% slower than the numbers in the README. What two things should you check before believing it?
<details><summary>Answer</summary>

That you're comparing on the same machine under the same conditions (measure the old code again, now, side by side), and that the binary really is built from the new code. If timings are noisy, use a deterministic measure such as callgrind's instruction count.
</details>
