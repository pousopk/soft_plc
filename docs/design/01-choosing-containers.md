# 1. Choosing containers

Counting in `src/`: about 120 `std::vector`s, 8 `std::map`s, 6 `std::unordered_map`s, 4 `std::set`s, 2 `std::deque`s, 4 `std::array`s and 32 `std::span`s. That ratio is deliberate. This note explains the default, then every case that isn't a vector, and why.

---

## The default: `std::vector`

A vector is one block of memory with the elements side by side. That gives it three properties nothing else has all at once:

- **Fast to walk.** The CPU loads memory in cache lines of 64 bytes and prefetches the next ones when you read in order. Walking a vector reads memory exactly in that order. A `std::map` or `std::list` puts every element in its own heap allocation, scattered: each step can be a cache miss, roughly a hundred times slower than a hit.
- **Indexable.** `values[17]` is one addition and one load. This is what the whole run-time design leans on (note 2): once a variable is a number, finding it is free.
- **Cheap to create and copy.** One allocation for the whole thing.

So the rule in this code is: **a vector unless there's a reason**. Some examples:

| Where | What | Why a vector is right |
|---|---|---|
| `Instance::values` (`src/st/interpreter.cpp`) | the memory of one PROGRAM or FB instance | indexed by leaf number, millions of times per second |
| `Pou::variables` (`src/st/ast.hpp`) | a POU's declarations, in order | the order is the declaration order, which the leaf numbers follow |
| `Chunk::code` (`src/st/bytecode.hpp`) | the instructions of one POU | the VM walks it in order and jumps by index |
| `ProcessImage::inputs_` / `outputs_` / `markers_` | the memory areas | they *are* arrays of bytes, addressed by byte offset |
| `StatementList` | the statements of a block | executed in order |

### Small n: a vector beats a map even for lookups

`findVariable` (`src/st/interpreter.cpp`) looks a variable up by name in a POU with a plain loop over the vector. A POU has tens of variables; comparing tens of short strings in memory that's side by side is as fast as hashing the name, and needs no extra structure to keep in sync. It's used by the monitor (`describe`, `READ`), by forces and by the online change, never by the program's own code.

A map starts to win when n is in the hundreds or more **and** the lookups are frequent. Below that, the plain loop is usually the faster code, and always the simpler one.

---

## `std::unordered_map`: many lookups by name, order irrelevant

All but one of the hash maps are in the **analyzer** (`src/st/analyzer.cpp`):

```cpp
std::unordered_map<std::string, const Pou*> functionBlocks_;      // upper-case name -> block
std::unordered_map<std::string, TypeDeclaration*> declaredTypes_; // upper-case name -> TYPE
std::unordered_map<std::string, std::size_t> functions_;          // upper-case name -> index
std::unordered_map<std::string, std::size_t> slots_;              // upper-case name -> variable
```

The analyzer resolves every name in the program: every variable reference, every type, every call. For a program with thousands of references and hundreds of names, a lookup per reference adds up, and a hash map makes each one O(1) on average. Nobody ever needs these names **in order**, so there's no reason to pay for a sorted `std::map`.

Two details:
- **The keys are upper-case**, because ST names are case-insensitive. The name is upper-cased once when it's stored and once per lookup, and then plain `std::string` hashing and equality do the rest. The alternative, a custom hash and equality that ignore case, avoids the copies but is more code to get right; at compile time the copies don't matter.
- **The values are pointers or indices**, not copies of the blocks: the map is an index into data that lives elsewhere (`CompilationUnit`).

The one hash map outside the analyzer is in the interpreter:

```cpp
std::unordered_map<const Pou*, Chunk> chunks_; // compiled code, per POU (Bytecode engine)
```

It's keyed by **pointer**: each POU's address is a unique, stable identity (note 3 explains why it's stable), and hashing a pointer is trivial. Ordering by address would mean nothing, so again no `std::map`.

### A cost left on the hot path

`run()` finds the code of a function block with `chunks_.at(instance.type)` **every time the block is called**: a hash lookup per FB call, per cycle. It's cheap (hash a pointer, compare it) and hasn't shown up as a problem, so it stayed; it was never measured on its own. If it ever matters, the fix is to look it up once, when the instance is created, and keep a `const Chunk*` in the `Instance`.

---

## `std::map`: when order or stability matters

A `std::map` is a sorted tree. It's slower than a hash map for lookups, so it has to earn its place with something a hash map doesn't give.

**Order you can see.** `StProgram::forces_` is a `std::map<std::string, Value>`. The monitor's `FORCES` reply lists them by walking the map, so they come out **sorted by name**, the same every time. A hash map would list them in an order that depends on the hash and can change between runs: harmless for a program, annoying for a person and fatal for a test that compares the reply with a string.

**Order that means something.** `dataBlocksUsedBy` returns a `std::map<std::uint16_t, std::size_t>`, data block number to size: the data blocks are created and printed in number order, DB1 before DB10.

**Stable element addresses.** `ProcessImage::dataBlocks_` is a `std::map<std::uint16_t, std::vector<std::uint8_t>>`. `dataBlock(n)` hands out a `std::span` that points into one of those vectors. An online change can add a data block while the program runs (`MonitorService::load`). In a map, inserting a new element **never moves the existing ones**, so spans into other data blocks stay valid. A `std::vector<std::vector<…>>` would reallocate when it grows, and every span into it would dangle.

(`std::unordered_map` also keeps element addresses stable when it grows, since it's node-based too; the map won here for the ordering.)

### Costs left on the hot path

- **Forces are re-resolved every cycle.** `applyForces` walks `forces_` and calls `setValue(path, …)`, which parses the path text and looks up each part, twice per cycle per force. With a few forces it's microseconds. The fix, if needed: resolve each path once when it's forced and keep the resolved place.
- **A data block access is a map lookup.** Every `AT %DB…` access goes through `region()` → `dataBlock(n)` → `dataBlocks_.find(n)`. A program with thousands of DB accesses per cycle would feel it; resolving the data block once, at compile time or when the program starts, would remove it.

These aren't oversights: they're the simple version, kept until a measurement says otherwise (principle 6).

---

## `std::set`: "have I seen this before?"

The analyzer uses `std::set` four times, always for the same question: is this name (or slot) a **duplicate**? Two fields with the same name in one STRUCT; the same input given twice in a call (`t(IN := a, IN := b)`). A set answers "insert, and tell me if it was already there" in one call:

```cpp
if (!names.insert(toUpper(field.name)).second) { /* STRUCT already has a field with this name */ }
```

These sets are tiny and short-lived, so sorted or hashed makes no measurable difference; `std::set` was the plain choice.

---

## `std::deque`: a queue

The monitor server's queue of requests (`src/monitor/monitor_server.hpp`) is a `std::deque`: client threads add at the back, the scan cycle takes from the front. A vector can do `push_back` but removing from the front moves everything after it; a deque does both ends in constant time. (`std::queue` is just an adapter over a deque; the code swaps the whole deque out under the lock, which `std::queue` doesn't allow.)

---

## `std::array`: a fixed size, no heap

Calling a FUNCTION gathers its arguments first:

```cpp
std::array<Value, kMaxCallArguments> inputs; // kMaxCallArguments = 32
```

A `std::vector` here would allocate on the heap on **every call**, every cycle. A `std::array` lives on the stack: no allocation at all. The price is a fixed limit, so the analyzer refuses a FUNCTION with more than 32 inputs. A PLC function with more than 32 parameters is a design problem anyway.

---

## `std::span`: a view, not a container

32 uses, and none of them own anything. A span is a pointer and a length: "these bytes, over there". `ProcessImage::bytes(Area::Output)` returns a span of the output area; `applyBuiltin` takes a span of its arguments. It lets a function accept "some contiguous values" without caring whether they're in a vector, an array or part of a bigger buffer, and without copying them.

The rule that comes with it: **a span must not outlive what it points at**. That's why the data block note above matters.

---

## Review questions

**1.** The monitor's `FORCES` reply lists forces in name order. What would change if `forces_` were an `std::unordered_map`, and who would notice?
<details><summary>Answer</summary>

The order would depend on the hash and could change from one run to another. The program wouldn't care (forces are applied all the same), but a person reading the list would see it shuffle, and the tests that compare the reply with an exact string would fail at random.
</details>

**2.** Why does the analyzer use hash maps for names, while `findVariable` in the interpreter uses a loop over a vector?
<details><summary>Answer</summary>

The analyzer looks up names constantly (every reference in the whole program) across hundreds of names, so O(1) lookups pay off. `findVariable` is only used by monitoring requests, in one POU's tens of variables: a loop is as fast and needs no extra structure. The running program uses neither: its names were turned into numbers at compile time (note 2).
</details>

**3.** `dataBlock(n)` returns a span into a vector stored in `dataBlocks_`. Why would changing `dataBlocks_` to `std::vector<std::vector<std::uint8_t>>` be dangerous once online changes can add data blocks?
<details><summary>Answer</summary>

Adding a data block could make the outer vector grow, which moves all the inner vectors to new memory. Any span handed out before (by a client, by the program) would then point at freed memory. A `std::map`'s elements never move when others are inserted.
</details>

**4.** Calling a FUNCTION gathers its arguments in a `std::array` of 32, not a `std::vector`. What does that save, and what does it cost?
<details><summary>Answer</summary>

It saves a heap allocation (and its free) on every call, every cycle: the array is on the stack. It costs a fixed limit of 32 inputs per FUNCTION, which the analyzer enforces.
</details>
