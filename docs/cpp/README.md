# C++ lessons: reading soft_plc's code

The milestone lessons in [`../lessons/`](../lessons/README.md) follow the order things were built and mix PLC and C++ topics. These lessons are the other way round: they take the finished code and explain the **C++** in it, one topic at a time, with the real code as examples. Each one ends with review questions; the answers are folded underneath.

Read them in order. Each builds on the ones before.

| # | Lesson | You'll understand |
|---|---|---|
| 1 | [Files, the build, and how `main()` starts the PLC](01-files-and-startup.md) | headers vs `.cpp`, namespaces, CMake targets, who owns which object, destruction order |
| 2 | [Classes, memory as bytes, interfaces and templates](02-classes-and-polymorphism.md) | `struct`/`class`, `enum class`, `vector`/`map`/`span`, bit operations, `virtual`, templates, factories |
| 3 | [Errors and cleanup](03-errors-and-raii.md) | `optional`, `expected`, exceptions, "throw inside, expected outside", RAII |
| 4 | [The syntax tree](04-syntax-tree.md) | `std::variant`, `std::visit`, `Overloaded`, `unique_ptr` trees, move, recursive descent, `string_view` |
| 5 | [Inside the interpreter and the VM](05-interpreter-and-vm.md) | slots, incomplete types, UB-free arithmetic, `bit_cast`, `union`, raw pointers, backpatching |
| 6 | [Threads, sockets and move-only objects](06-threads-and-sockets.md) | `mutex`, `promise`/`future`, `atomic`, `jthread`, POSIX sockets, move constructors |
| 7 | [How the tests are built](07-testing.md) | doctest, fakes, injected time, table-driven and differential tests, sanitizers |

## Where to find each C++ feature

| Feature | Standard | Best example | Lesson |
|---|---|---|---|
| `std::unique_ptr`, `std::make_unique` | C++11/14 | `src/st/ast.hpp` (`ExpressionPtr`), `src/main.cpp` | 1, 4 |
| References vs owning members | — | `src/core/scan_cycle.hpp` | 1 |
| `std::optional` | C++17 | `src/main.cpp` (`Options`) | 1, 3 |
| `if` with initialiser | C++17 | `src/main.cpp` (`parseOptions`) | 1 |
| Lambdas and captures | C++11 | `src/main.cpp` (idle task) | 1, 2 |
| `enum class` | C++11 | `src/core/address.hpp` | 2 |
| `operator== = default` | C++20 | `src/core/address.hpp` | 2 |
| `std::span` | C++20 | `src/core/process_image.hpp` | 2 |
| `std::as_const` + `const_cast` | C++17 | `ProcessImage::dataBlock` | 2 |
| Virtual functions, `override` | — | `src/core/program.hpp`, `src/st/builtins.hpp` | 2 |
| Class templates | — | `TimerBlock` in `src/st/builtins.cpp` | 2 |
| Function-local `static` | C++11 | `builtinFunctionBlocks()` | 2 |
| Raw string literals | C++11 | `kInterfaces` in `src/st/builtins.cpp` | 2 |
| `tl::expected` (≈ `std::expected`, C++23) | library | `src/core/address.cpp`, `src/st/parser.hpp` | 3 |
| `[[noreturn]]` | C++11 | `Parser::fail` | 3 |
| RAII guards | — | `AreaLock`, `ImageBinding`, `FrameGuard` | 3 |
| `std::variant`, `std::visit` | C++17 | `src/st/ast.hpp`, `src/st/ast.cpp` | 4 |
| Variadic templates, deduction guides | C++11/17 | `src/util/overloaded.hpp` | 4 |
| Generic lambdas (`auto` parameters) | C++14 | `Analyzer::checkStatements` | 4 |
| Pointer to member function | — | `Parser::binaryLevel` | 4 |
| `std::initializer_list`, `constexpr` tables | C++11 | `src/st/parser.cpp`, `src/st/lexer.cpp` | 4 |
| `std::string_view` | C++17 | `src/st/lexer.cpp` | 4 |
| Incomplete types + out-of-line destructor | — | `src/st/interpreter.hpp` | 5 |
| Avoiding signed-overflow UB | C++20 rules | `src/st/runtime_values.hpp` | 5 |
| `std::bit_cast` | C++20 | `Interpreter::load` | 5 |
| `union` | — | `VmSlot` in `src/st/interpreter.hpp` | 5 |
| `std::mutex`, `std::lock_guard` | C++11/17 | `src/monitor/monitor_server.cpp` | 6 |
| `std::promise`, `std::future` | C++11 | `MonitorServer::ask` | 6 |
| `std::atomic` | C++11 | `stopping_`, `stopRequested` | 6 |
| `std::jthread`, `std::stop_token` | C++20 | `tools/monitor_gui/poller.cpp` | 6 |
| `mutable` | — | `Poller::mutex_` | 6 |
| Move constructor, `std::exchange`, `noexcept` | C++11/14 | `src/monitor/monitor_client.cpp` | 6 |
| doctest, fakes, `std::mt19937` | library/C++11 | `tests/` | 7 |
