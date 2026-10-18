# 2. Names at compile time, numbers at run time

A PLC runs the same code every few milliseconds, forever. Anything it can avoid doing in each cycle, it should. The biggest thing soft_plc avoids is **looking up names**: by the time the program runs, `count`, `t1.Q` and `cameras[2].score` are all just numbers.

```
source text        count := count + 1;
   │ lexer, parser
syntax tree        Assignment{ Access{name:"count"}, Binary{+, Access{name:"count"}, 1} }
   │ analyzer      "count" is variable 0 of Main, an INT, leaf 0
syntax tree        Access{name:"count", slot:0, directLeaf:0, type:INT}
   │ compiler
bytecode           LoadInt 0 · PushInt 1 · AddI · Wrap16 · StoreInt16 0
   │ every cycle
VM                 values[0] = wrap16(values[0] + 1)
```

---

## The analyzer fills in the numbers

The parser only knows text: an `Access` (`src/st/ast.hpp`) starts life as a name and a list of selectors (`.Q`, `[i]`, `.x`). The analyzer resolves it once, using its hash maps (note 1), and writes the answer **into the same node**:

```cpp
std::size_t instanceSlot; // fb.member: which instance
std::size_t slot;         // which variable
std::size_t leafOffset;   // the fixed part of the offset inside it
std::vector<IndexTerm> dynamic; // indices only known at run time: values[i]
std::size_t directLeaf;   // the common case: one fixed leaf, ready to use
```

So `cameras[2].score` becomes "variable 5, leaf offset 17", a constant worked out from the struct's layout and the array's bounds, and `values[i]` becomes "variable 3, plus `(i - low) * stride`, checked against the bounds". The interpreter never sees the names again.

### Alternatives that weren't taken

- **Look names up while running.** A map from name to value, consulted on every access. It's how a quick scripting language starts, and it would make every `count := count + 1` two hash lookups. For a PLC doing this thousands of times per cycle, that's the cost that matters most, paid forever.
- **Resolve into separate structures.** The analyzer could build a second tree for the interpreter, leaving the parser's tree untouched. Cleaner in theory, but twice the node types to keep in step. Filling in the parser's nodes is simpler: one tree, which gains information as it goes through the stages.

### What it costs

The work moves to the analyzer, which is the biggest file in the project (`src/st/analyzer.cpp`, about 1300 lines). That's the right place for complexity: it runs once, and it's also where errors are found, so the same walk that resolves `count` can say "unknown variable 'cuont'" with a line and column.

The names don't disappear: they're still in the tree and in `Pou::variables`, because the monitor needs them to answer `READ cameras[2].score`. Going from a path back to a place (`resolvePath` in `src/st/interpreter.cpp`) is the slow, rare direction, done per request, not per cycle.

---

## Leaves: one flat memory per instance

Each PROGRAM or function block instance has its memory in **one `std::vector<Value>`**, `Instance::values`. Every elementary value inside it is one element, a **leaf**:

```
VAR                                      values: [ count | p.x | p.y | arr[1] | arr[2] | arr[3] ]
    count : INT;                                     0       1     2      3        4        5
    p : Point;             (* x, y : REAL *)
    arr : ARRAY[1..3] OF INT;
END_VAR
```

A struct is its fields' leaves one after the other, an array its elements' leaves one after the other, nested as deep as needed. The analyzer computes each variable's first leaf and each field's offset once (`Type::Field::leafOffset`, `Type::elementLeaves`).

Why one flat row, instead of a tree of objects mirroring the types?

- **Every access is one index.** A field of an element of an array of structs is `first leaf + constant + i × stride`. No pointers to follow.
- **Copying is a loop over a range.** `a := b;` for two arrays of structs copies leaf `k` of one to leaf `k` of the other, for every `k` (`copyAggregate`, which goes through the normal load and store so it also works when one side is an `AT` variable). The online change keeps a variable's value the same way, with a straight `std::copy_n` of its leaves from the old memory to the new (`takeOverInstance`).
- **One kind of storage.** Initialization, monitoring, forcing and the online change all work on "leaf n of instance i". None of them needs a case per type.

### Two layouts, on purpose

Variables placed with `AT` don't live in the leaves at all: they live in the **process image**, in the bytes and bits an S7 client sees, laid out with Siemens' rules (BOOLs packed, INTs on even bytes, big-endian). So every type has **two** layouts, both computed by the analyzer: leaves for the program's own memory, bits for the process image (`Type::leaves`, `Type::bitSize`).

One layout would have been simpler to explain. But the S7 layout is awkward to work with (bit offsets, byte order, padding), and only `AT` variables need it; everything else gets the simple, fast one. The `Access` carries both offsets (`leafOffset` and `bitOffset`), and the interpreter picks the one that applies.

---

## `Value` and `VmSlot`: safe by default, fast where it's proven

A leaf is a `Value`, a `std::variant<bool, std::int64_t, double>`. Every integer type (INT, DINT, TIME) is held in 64 bits and wrapped to its size after each operation; every REAL in a double, rounded to float precision. Three cases instead of five keep the arithmetic code small, and the variant knows which one it holds, so a mistake (reading a REAL as an INT) is caught instead of silently producing garbage.

The bytecode VM's stack uses something rawer:

```cpp
union VmSlot {        // src/st/interpreter.hpp
    std::int64_t i;
    double r;
};
```

No tag: the slot doesn't know what it holds. That's only safe because **the compiler knows**: the analyzer worked out every expression's type, so the compiler emits `AddI` for integers and `AddR` for REALs, and the VM never has to check (`src/st/bytecode.hpp`). The tree-walking interpreter, which checks everything through the variant, is still there, and the whole test suite runs on **both** engines (`ctest` runs it twice) plus a differential test that runs the same programs on both, with random inputs, and compares every variable after every cycle. The fast engine is trusted *because* the slow one exists to compare against.

This is the general shape of an optimization here: keep the simple version, build the fast one next to it, and prove them equal with tests.

---

## Review questions

**1.** After analysis, what does the interpreter need to execute `x := cameras[2].score;`, and what does it no longer need?
<details><summary>Answer</summary>

It needs the slots and the leaf offset the analyzer stored in each `Access`: variable number and a constant offset for `cameras[2].score`, the slot and leaf of `x`. It no longer needs any names, maps, or the struct's definition: the offset was computed once from the layout.
</details>

**2.** `values[i]` can't be resolved to a single leaf at compile time. What does the analyzer store for it, and what's left for run time?
<details><summary>Answer</summary>

A dynamic index term: the array's bounds and the stride (leaves per step). At run time the interpreter evaluates `i`, checks it against the bounds (STOP if outside), and adds `(i - low) × stride` to the variable's first leaf.
</details>

**3.** Why does a STRUCT have two layouts, and which one does a variable without `AT` use?
<details><summary>Answer</summary>

Leaves for the program's own memory (simple, one value per element, fast to index and copy) and the S7 bit layout for the process image (what S7 clients expect: packed BOOLs, even-byte alignment, big-endian). A variable without `AT` uses leaves only.
</details>

**4.** The VM's `VmSlot` is an untagged union: reading `r` when `i` was written gives garbage. Why is that acceptable here, and what guards it?
<details><summary>Answer</summary>

The analyzer types every expression and the compiler picks typed instructions (`AddI`, `AddR`), so the VM always reads a slot as what was written. That's guarded by tests: the whole suite runs on both the VM and the tree-walker (which uses the checked `std::variant`), and a differential test compares them cycle by cycle, with random inputs.
</details>
