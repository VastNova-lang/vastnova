# VastNova Compiler

VastNova is a lightweight, statically typed programming language that compiles directly to native executables via LLVM. It features a clean syntax, type inference, and a minimal runtime – ideal for scripting, embedded systems, or learning compiler construction.

## Quick Start

### Prerequisites
- C++17 compiler (GCC, Clang, or MSVC)
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

### Setting the Standard Library

VastNova uses an optional standard library for common definitions (e.g., `bool.vn`). By default, the compiler looks for a `std/` directory in the current working directory. To use a custom location, set the `VASTNOVA_STD` environment variable:

```bash
export VASTNOVA_STD=/path/to/your/std
```

If not set, the compiler will fall back to `./std/` relative to the current directory.

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

## Documentation

Detailed language documentation, compiler internals, and contribution guidelines are available in the [Docs/](Docs/) folder.

## License

MIT License