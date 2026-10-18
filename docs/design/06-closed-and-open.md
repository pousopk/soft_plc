# 6. Closed sets and open sets

Whenever code has to handle "one of several kinds of thing", there are two classic C++ answers:

- **`std::variant`**: the thing *is one of* a fixed list of types, and code that handles it uses `std::visit` with a case for each.
- **`virtual` functions**: the thing *is some* class derived from a base, and calls go through the base's interface to whichever class it is.

They look interchangeable, but they make opposite things easy. soft_plc uses both, and the choice each time follows one question: **is the list of kinds fixed, or is the list of operations fixed?**

|  | Adding a new **kind** | Adding a new **operation** |
|---|---|---|
| `std::variant` | hard: every `visit` must learn the new case (the compiler finds them all for you) | easy: write one more visitor, no existing type changes |
| `virtual` | easy: write one more derived class, nothing else changes | hard: every derived class must implement it |

(This trade-off has a name: the *expression problem*.)

---

## The syntax tree: kinds fixed, operations many → `std::variant`

```cpp
// src/st/ast.hpp
std::variant<Literal, Access, CallExpression, UnaryExpression, BinaryExpression> node;  // an expression
std::variant<Assignment, IfStatement, CaseStatement, ForStatement, ...> node;           // a statement
```

The kinds of statement and expression are fixed by the **language**: adding one (a `CONTINUE` statement) is rare, and it's a language change anyway, touching the parser, the analyzer and both engines. Meanwhile there are **many operations** on the tree: analyze it, interpret it, compile it to bytecode, collect its names, print it. And `ladder_editor` reads the tree as a library.

With `virtual`, each of those operations would be a virtual function on every node class: `analyze()`, `interpret()`, `compile()`... the analyzer's logic would be scattered across thirty node classes, and the AST header would depend on the analyzer, the interpreter and the compiler. With `variant`, each operation is one place: the analyzer is one file with a case per node, the compiler another. And when a new node kind *is* added, `std::visit` with `Overloaded` (`src/util/overloaded.hpp`) refuses to compile anywhere the new case isn't handled. The compiler gives you the to-do list.

A practical bonus: variants hold their alternatives **by value**, so there's no heap allocation and no pointer per node (only the recursive children, `ExpressionPtr`, are on the heap).

The same reasoning gives the other variants: `Value` (a PLC value is one of three representations, a fixed set), `Selector` (`.field` or `[index]`), `Initializer` (a value, an array list, a struct list), and the lexer's `Token::value`.

---

## The PLC's parts: operations fixed, kinds open → `virtual`

```cpp
class Program {            // src/core/program.hpp
public:
    virtual ~Program() = default;
    virtual void execute(ProcessImage& image, const CycleContext& context) = 0;
};

class IoDriver {           // src/core/io_driver.hpp
public:
    virtual void readInputs(ProcessImage& image) = 0;
    virtual void writeOutputs(const ProcessImage& image) = 0;
};
```

Here it's the other way round. The **operations** are fixed by what a PLC is: a program executes once per cycle; an I/O driver reads inputs and writes outputs. The **kinds** keep growing, and from places the scan cycle shouldn't know about:

- programs: the ST program (`StProgram`), the C++ blink demo (`BlinkProgram`);
- I/O drivers: the console (`ConsoleIo`), three test fakes (`NullIo` twice, `FakeIo`), the simulator's no-I/O driver in `ladder_editor`, and a `factory_sim` driver some day.

With `virtual`, each new kind is a new class somewhere else, and `ScanCycle` doesn't change or even recompile. A variant of all programs would make `scan_cycle.hpp` include every one of them, tests' fakes included. A virtual call costs an indirect jump, three times per cycle: nothing.

This is what makes the scan cycle **testable**: `tests/core/scan_cycle_test.cpp` hands it a `FakeIo` that records what was written, and checks the order of operations, with no hardware.

---

## The standard blocks: a closed set, chosen as open

```cpp
class NativeBlock {        // src/st/builtins.hpp
public:
    virtual void execute(std::vector<Value>& variables, TimePoint now) = 0;
};
template <class Timer> class TimerBlock : public NativeBlock { ... };  // TON, TOF, TP
```

Here the honest answer is "either would work". The set of standard blocks is **closed** (TON, TOF, TP, CTU, CTD, R_TRIG, F_TRIG, SR, RS), which argues for a variant: `std::variant<Ton, Tof, Tp, Ctu, …>` held by value in the instance, with no heap allocation per instance.

`virtual` was chosen because:
- adding a standard block is then a self-contained change (a class, and one line in `createBuiltin`), which matters when more get added (`TONR`, `CTUD`…);
- the interpreter only needs "run this block", one operation, which is where `virtual` is at its best;
- the cost, one heap allocation per standard block **instance**, is paid once, when the program starts (or at an online change), never per cycle.

The design note worth remembering: when both fit, choose by **which change you expect**, and check the cost lands somewhere it doesn't matter.

---

## Templates: the same code for several types, decided at compile time

`TimerBlock<Timer>` adapts TON, TOF and TP, which share an interface (IN, PT → Q, ET), with one piece of code:

```cpp
template <class Timer> class TimerBlock : public NativeBlock {
    void execute(std::vector<Value>& v, TimePoint now) override {
        timer_.update(boolAt(v, 0), toDuration(intAt(v, 1)), now);
        v[2] = timer_.q();
        v[3] = toMilliseconds(timer_.et());
    }
    Timer timer_;
};
```

`Ton`, `Tof` and `Tp` share no base class; the template only needs them to have `update`, `q()` and `et()`. That's **compile-time polymorphism**: the compiler generates one `TimerBlock` per timer, each calling its timer's functions directly. Templates are used for this kind of small, local sharing of code. They're not used for big public interfaces, where error messages get long and every user has to see the whole implementation in the header.

---

## `std::function`: "call me back", with no class to write

Three places need to be told what to do by whoever creates them:

```cpp
void ScanCycle::setIdleTask(std::function<void()> task);          // what to do between cycles
using StopHandler = std::function<void(const RuntimeError&)>;     // what to do on STOP
using Handler = std::function<std::string(std::string_view)>;     // how to answer a request
```

Each is a single callback, set once, called from one place. An interface class with one virtual function would work too, but every caller would have to write a class; a lambda that captures what it needs is one line in `main()`:

```cpp
plc.setIdleTask([&] { server->processPending(...); s7->synchronize(); });
```

`std::function` costs a possible allocation when it's set and an indirect call when it's called: once per cycle, negligible. It would be the wrong tool inside the interpreter's inner loop.

---

## Review questions

**1.** You add a `CONTINUE` statement to the language. With the variant design, how do you find every place that has to handle it?
<details><summary>Answer</summary>

Add `ContinueStatement` to the statement variant and build: every `std::visit` with `Overloaded` that doesn't have a case for it fails to compile. The compiler lists the analyzer, both engines, and any other visitor that needs updating.
</details>

**2.** `factory_sim` will need an I/O driver that exchanges the process image with the simulation. What changes in the scan cycle?
<details><summary>Answer</summary>

Nothing. The new driver derives from `IoDriver`, implements `readInputs` and `writeOutputs`, and `main()` passes it to `ScanCycle` instead of `ConsoleIo`. That's exactly the kind of change `virtual` makes easy.
</details>

**3.** The standard blocks are a closed set, which usually suggests a variant. Why are they virtual, and when does the cost of that choice get paid?
<details><summary>Answer</summary>

Because adding a block is then self-contained (a class and a line in the factory), and the interpreter needs only one operation, "execute". The cost, a heap allocation per standard block instance, is paid when the program starts or is changed online, never in the cycle.
</details>

**4.** Why is the idle task a `std::function` and not an `IdleTask` interface with a virtual `run()`?
<details><summary>Answer</summary>

It's a single callback, set once and called once per cycle. A lambda capturing the monitor server and the S7 server is one line in `main()`; an interface would need a class written just to hold those two pointers. The cost difference is negligible at once per cycle.
</details>
