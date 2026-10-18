# Design notes: the thinking behind soft_plc

The [C++ lessons](../cpp/README.md) explain **what** each language feature in the code is. These notes explain **why** the code is shaped the way it is: the decisions, what else was possible, and what each choice costs. They're the conversations a team would have at a whiteboard before writing the code, written down afterwards.

Each note points at the real code (`file:line` or the function name), and ends with review questions; the answers are folded underneath.

| # | Note | The question it answers |
|---|---|---|
| 1 | [Choosing containers](01-choosing-containers.md) | vector, map, unordered_map, set, deque, array, span: which, where, and why |
| 2 | [Names at compile time, numbers at run time](02-names-and-numbers.md) | why the running PLC never looks up a name, and the memory model that makes it possible |
| 3 | [Who owns what](03-ownership.md) | unique_ptr trees, raw pointers that are safe, references, and objects that must not move |
| 4 | [Three kinds of errors](04-errors.md) | a mistake in the user's program, a fault while it runs, a bug in soft_plc: three different tools |
| 5 | [One thread runs the PLC](05-one-thread.md) | why the interpreter has no locks, and how the network gets in anyway |
| 6 | [Closed sets and open sets](06-closed-and-open.md) | `std::variant` or `virtual`, templates or `std::function` |
| 7 | [Text at the edges](07-text-at-the-edges.md) | a line-based protocol, errors with line and column, a guide that's tested |

## The principles, in one place

These come back in every note:

1. **Do the work once, early.** Anything that can be worked out when the program is compiled (where a variable lives, what type an expression has, which instruction to run) is worked out then, so the scan cycle only does what can't be known in advance.
2. **Pick the plainest structure that's correct, then measure.** A `std::vector` and a loop are the default. Something cleverer has to earn its place with a reason: an ordering that matters, a lookup that happens a lot, a lifetime that needs it.
3. **One owner per thing, and lifetimes you can see.** Every object has one owner; everything else borrows it, and the borrowers die first.
4. **Keep the PLC deterministic.** The same program, the same inputs and the same clock give the same outputs. That's what makes a PLC trustworthy, and it's also what makes it testable.
5. **Behave like a PLC where it matters.** STOP on a fault, outputs off, changes only between cycles, Siemens memory layout. Where soft_plc differs from an S7, it says so.
6. **Leave the costs where you can see them.** Some lookups are still on the hot path because they're cheap enough today. The notes point them out, with how to remove them when they stop being cheap.
