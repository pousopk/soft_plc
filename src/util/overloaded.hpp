#pragma once

namespace softplc {

// Builds one callable out of several lambdas, so std::visit can pick the
// right lambda for each type a std::variant may hold:
//
//   std::visit(Overloaded{
//       [](int i) { ... },
//       [](const std::string& s) { ... },
//   }, value);
template <class... Lambdas> struct Overloaded : Lambdas... {
    using Lambdas::operator()...;
};

// Deduction guide: lets you write Overloaded{...} without listing the lambda
// types. C++20 deduces this on its own; it's kept for clarity and older compilers.
template <class... Lambdas> Overloaded(Lambdas...) -> Overloaded<Lambdas...>;

} // namespace softplc
