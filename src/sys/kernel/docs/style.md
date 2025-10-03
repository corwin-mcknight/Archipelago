# Coding Style

Utilizing the .editorconfig and .clang-format files in the root directory is highly recommended to maintain consistent formatting across the codebase.

Editor Setup:
* Use 4 spaces for indentation, no tabs.
* Limit lines to 120 characters.

Coding in C++:
* Variables, functions, methods, files, directories, namespaces, and types should use `snake_case`.
* Classes and structs should use `CamelCase`.
* Constants and macros should use `UPPER_SNAKE_CASE`.
* Use `PascalCase` for enum types and `UPPER_SNAKE_CASE` for enum values.
* Use `this->` to access member variables and methods within class methods.
* Comment only with `//` for single line comments and `/* ... */` for multi-line comments.
* For comments, only document non-obvious behavior. Otherwise, the code should be self-documenting.
* Use C++ features and RAII where appropriate, but avoid complex metaprogramming or template-heavy code if possible
* Use `nullptr` instead of `NULL` or `0` for null pointers.
* Use `constexpr` and `const` where appropriate.
* Prefer composition over inheritance, except where it simplifies code significantly or interfaces are required.