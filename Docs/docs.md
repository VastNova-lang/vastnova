# VastNova Language Reference

VastNova is a statically typed, compiled language designed for simplicity and performance. This document describes every aspect of the language in detail.

## 1. Comments

VastNova supports three forms of comments:

- `//` – single-line comment (C++ style)
- `#` – single-line comment (shell style)
- `!# ... #!` – multi-line comment (blocks can span any number of lines)

```vastnova
// this is a comment
# this is also a comment
!# this is a multi‑line comment
   it can contain anything
   until the closing marker #!
```

## 2. Types

VastNova has four primitive types:

| Type  | Description               | Example literals |
|-------|---------------------------|------------------|
| `i32` | 32‑bit signed integer     | `42`, `-5`       |
| `i64` | 64‑bit signed integer     | `1000000`        |
| `f64` | 64‑bit IEEE‑754 float     | `3.14`, `-0.001` |
| `str` | UTF‑8 string (heap‑allocated) | `"Hello"`   |

All types are **static** but can be **inferred** when a value is given.

## 3. Variables and Constants

### Variable Declaration

```vastnova
var name             // declared but uninitialized (defaults to 0 for numbers, empty for str)
var name = value     // type inferred from value
var name : type      // explicit type, no initial value
var name : type = value  // explicit type with initial value
```

Examples:
```vastnova
var a                 // a is i32, value = 0
var b = 42            // b is i32
var c : f64 = 3.14    // c is f64
var d : str = "text"  // d is str
```

### Constants

Constants are declared with `let` and **must** be initialized. They cannot be reassigned later.

```vastnova
let pi : f64 = 3.14159
let name = "Alice"    // type inferred as str
```

## 4. Arithmetic Expressions

Operators: `+` `-` `*` `/` with standard precedence (multiplication/division before addition/subtraction). Parentheses can override precedence.

All arithmetic is **type‑aware**:
- If both operands are integers, result is integer (truncating division).
- If either operand is float, result is float.
- String `+` performs **concatenation** (only between strings).

Examples:
```vastnova
var a = 10 + 5        // 15 (i32)
var b = 3.14 * 2      // 6.28 (f64)
var c = 20 / 3        // 6 (integer division)
var d = "Hello" + " " + "World"  // "Hello World" (str)
```

Note: Mixing numbers and strings in `+` is **not** allowed; use `str()` to convert numbers explicitly.

## 5. Output: `print`

`print` takes any number of arguments (numbers, strings, variables) and prints them separated by spaces, ending with a newline. It automatically chooses the correct formatting.

```vastnova
print("Hello")               // prints: Hello
print(a, b, c)               // prints the values of a, b, c separated by spaces
print("Result:", a + b)      // mixed literals and expressions
```

## 6. Input: `input`

`input` reads a line from standard input and returns it as a `str`. It can take an optional prompt argument (printed before reading).

```vastnova
var name = input("Enter your name: ")   // prints prompt, then reads
var age = input()                       // reads without prompt
print("Hello,", name, "age:", age)
```

`input` always returns a string. To perform numeric operations, convert the result using `int()` or `float()`.

## 7. Conditionals: `if` / `else`

The `if` statement evaluates a condition and executes the `then` block if true; optionally an `else` block runs if false.

**Condition syntax:**  
Conditions can use comparison operators `>`, `<`, `>=`, `<=`, `==`, `!=` and logical operators `&&` (AND), `||` (OR). Parentheses are **not** required around the condition.

```vastnova
var score = int(input("Enter your score: "))
if score >= 90 {
    print("Excellent")
} else if score >= 60 {
    print("Pass")
} else {
    print("Fail")
}
```

Both `then` and `else` blocks **must** be enclosed in `{ }`, even for a single statement.

## 8. Loops: `while`

The `while` loop repeatedly executes its body as long as the condition evaluates to true. The condition is checked before each iteration.

```vastnova
var i = 0
while i < 5 {
    print("i =", i)
    i = i + 1
}
```

### Loop Control: `break` and `continue`

- `break` – immediately exits the innermost loop.
- `continue` – skips the rest of the current iteration and jumps to the condition check.

```vastnova
var i = 0
while i < 10 {
    i = i + 1
    if i == 3 {
        continue   // skip printing 3
    }
    if i == 7 {
        break      // stop at 7
    }
    print(i)       // prints: 1,2,4,5,6
}
```

`break` and `continue` can only appear inside a loop body; using them outside results in a compile‑time error.

## 9. String Operations

### Concatenation (`+`)
Two strings can be concatenated with `+`. The result is a newly allocated string.

```vastnova
var first = "Hello"
var last = "World"
var full = first + " " + last   // "Hello World"
```

### Conversion from numbers (`str()`)
The built‑in function `str(number)` converts an integer or float to its string representation. This is useful for embedding numbers in strings.

```vastnova
var pi = 3.14159
var pi_str = str(pi)          // "3.14159"
var msg = "Pi is " + pi_str
print(msg)                   // Pi is 3.14159
```

### Conversion from strings (`int()`, `float()`)
These built‑in functions convert a string (or number) to an integer or float. They are essential for processing user input numerically.

```vastnova
var age_str = input("Enter your age: ")
var age = int(age_str)        // convert to i32
var weight_str = input("Enter your weight: ")
var weight = float(weight_str) // convert to f64
print("Next year you will be", age + 1)
```

If the argument is already a number, `int()` and `float()` simply cast it to the desired type.

## 10. Variable Reassignment

Variables declared with `var` can be reassigned at any time. The new value must be compatible with the variable's type (no implicit conversions between incompatible types).

```vastnova
var x = 10
x = 20          // OK
x = "text"      // ERROR: type mismatch (i32 vs str)
```

Constants (`let`) cannot be reassigned.

## 11. Modules and Imports

VastNova supports a simple module system via the `import` statement.

### Syntax
```vastnova
import "path/to/file.vn"
```

The path can be relative or absolute. The compiler searches for the file in the following order:

1. If the `VASTNOVA_STD` environment variable is set, it looks for the file under that directory first.
2. Otherwise, it looks for `std/` in the current working directory.
3. Finally, it looks for the file as given (relative to the current working directory or absolute path).

Duplicate imports are ignored – each file is loaded only once.

### Example: Using Boolean Constants
Create `std/bool.vn`:
```vastnova
let true = 1
let false = 0
```

Then use it in your program:
```vastnova
import "bool.vn"

var flag = true
if flag == true {
    print("It's true!")
}
```

### Setting the Standard Library Path
You can set the `VASTNOVA_STD` environment variable to point to your standard library directory:
```bash
export VASTNOVA_STD=/path/to/std
```

If not set, the compiler falls back to `./std/`.

## 12. Complete Example Program

Below is a full program that uses almost every feature:

```vastnova
// Input and output
var name = input("What is your name? ")
var age_str = input("How old are you? ")
var age = int(age_str)

// Arithmetic
var birth_year = 2025 - age
var next_age = age + 1

// Conditions
if age >= 18 {
    print("You are an adult.")
} else {
    print("You are a minor.")
}

// String concatenation with number conversion
var message = "Hello, " + name + "! You will be " + str(next_age) + " next year."
print(message)

// Using float conversion
var weight_str = input("Enter your weight in kg: ")
var weight = float(weight_str)
print("Your weight is", weight, "kg")

// Loop example
var i = 0
while i < 5 {
    print("i =", i)
    i = i + 1
}
```

## 13. Errors and Limitations

- No arrays or dictionaries yet.
- No user‑defined functions.
- No explicit type casting except `str()`, `int()`, `float()`.
- All `if`/`else` and `while` blocks must use `{ }`; no single‑line shorthand.
- `break` and `continue` can only appear inside loops.
- Imported files are loaded at compile time; no dynamic imports.

## 14. Further Reading

For compiler internals and contribution guide, check the project's main documentation.