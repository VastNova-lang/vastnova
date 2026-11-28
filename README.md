# VastNova Compiler

VastNova is a lightweight, statically typed programming language that compiles directly to native executables via LLVM. It features a clean syntax, type inference, and a minimal runtime – ideal for scripting, embedded systems, or learning compiler construction.

## Quick Start

### Prerequisites
- C++17 compiler (GCC or Clang)
- LLVM (≥ 14) with development headers
- `clang` (to compile generated IR)
- `llvm-config`

### Build
Clone the repository and run the build script:

```bash
git clone https://github.com/VastNova-lang/vastnova.git
cd vastnova
./build.sh
```

This produces the `vastnova` executable in the project root.

### Compile and Run a Program
Create a file `hello.vn`:

```vastnova
print("Hello, World!")
```

Then compile and execute:

```bash
./vastnova hello.vn hello
./hello
```

Output:
```
Hello, World!
```

For more examples, see the [examples/](examples/) directory.

### Run Tests
Build the compiler first, then run the test script:

```bash
./build.sh
sh test/run_tests.sh
```

The script runs the programs in `test/cases/` and compares their output with the
matching `.expected` files. The cases cover variables and scope, arithmetic,
comparisons and logic, control flow, functions and recursion, arrays, strings and
escapes, `input`, imports, comments, arbitrary integer widths, and all four
float formats. A case with a matching `.stdin` file receives that file on
standard input.

The programs in `test/errors/` must fail with the diagnostic listed in the
matching `.error` file. All intermediate files are written under `test/temp/`,
which is ignored by git and cleaned after each run. Pass a compiler path as the
first argument or set `VASTNOVA` to test a different build.

## Documentation

For language syntax, compiler internals, and contribution guidelines, see the [language reference](Docs/docs.md).

## License

MIT License
