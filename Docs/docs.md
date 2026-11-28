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
!# this is a multi-line comment
   it can contain anything
   until the closing marker #!
```

## 2. Types

VastNova has arbitrary-width integers, four floating-point types, `str`, and fixed-length arrays.

Integer types are written `iN` (signed) or `uN` (unsigned), where `N` is a positive bit width.
Any width from 1 to 65535 is accepted; there is no fixed list of integer types.

| Type  | Description                         | Range / example values |
|-------|-------------------------------------|------------------|
| `iN`  | signed two's-complement integer     | `i3` is -4..3; `i32` is -2147483648..2147483647 |
| `uN`  | unsigned integer                    | `u3` is 0..7; `u8` is 0..255 |
| `f16` | 16-bit IEEE-754 float               | `1.5`, `-0.25` |
| `f32` | 32-bit IEEE-754 float               | `1.5`, `-0.25` |
| `f64` | 64-bit IEEE-754 float (default)     | `3.14`, `-0.001` |
| `f128`| 128-bit IEEE-754 float              | `2.5`, `-1.0e10` |
| `str` | UTF-8 string (heap-allocated)       | `"Hello"` |

Only `f16`, `f32`, `f64`, and `f128` are valid float types. `f9`, `f18`, and every other
float width are compile errors.

All types are **static** but can be **inferred** when a value is given.

A bare integer literal that fits in `i32` is `i32`; larger literals are `i64`, and
literals above `i64` range are `u64`. An integer literal that does not fit in `u64`
is a compile error. Literals containing a `.` are `f64`.

```vastnova
var a : i3 = 2
var b : i18 = 100000
var c : u8 = 200
var d : u3 = 7
var e : i128 = int(18446744073709551615, 128)
var f : f32 = float(1.5, 32)
```

## 3. Variables and Constants

### Variable Declaration

```vastnova
var name             // declared and initialized to 0 (numbers) or "" (str)
var name = value     // type inferred from value
var name : type      // explicit type, initialized to the default value
var name : type = value  // explicit type with initial value
```

Examples:
```vastnova
var a                 // a is i32, value = 0
var b = 42            // b is i32
var c : f64 = 3.14    // c is f64
var d : str = "text"  // d is str
var e : i64 = 5000000000
```

Variables declared without an initializer are zero-initialized (`0` for numbers, `""` for strings, all-zero elements for arrays).

### Scope

Every function body, `if`/`else` block, and `while` body is a scope. A variable declared inside a block is visible only inside that block. An inner scope may shadow an outer variable with the same name; declaring the same name twice in the same scope is a compile error.

### Constants

Constants are declared with `let` and **must** be initialized with a compile-time constant expression. They cannot be reassigned later.

```vastnova
let pi : f64 = 3.14159
let name = "Alice"    // type inferred as str
let x = 10 + 2        // folded to 12 at compile time
```

Assigning to a constant is a compile error. Since `true` and `false` are not keywords, they are provided as constants by `std/bool.vn`.

## 4. Arrays

Arrays are fixed-length sequences of a single element type. The length and the element type are both part of the type.

### Declaration and Initialization

```vastnova
var a : i32[10]                  // 10 i32 elements, all initialized to 0
var b : i32[5] = [1, 2, 3, 4, 5]  // explicit type, length must match
var c = [1, 2, 3]                // inferred as i32[3]
var d : f64[3] = [1.0, 2.0, 3.0]
var names : str[3] = ["a", "b", "c"]
```

When the type is omitted, it is inferred from the literal using the same rules as
mixed arithmetic:

- all integer elements with the same signedness → that signedness at the widest element width
- signed and unsigned integer elements mixed → compile error
- any floating-point element with an integer element → `f64`
- all floating-point elements → the widest float width
- all string elements → `str[n]`
- mixing strings and numbers in one literal is an error

```vastnova
var a = [1, 2, 3]                  // i32[3]
var b = [1, 2, 3, 4000000000]      // i64[4]
var c : u8[3] = [200, 100, 50]     // explicit unsigned element type
var d : i3[3] = [1, 2, 3]          // explicit 3-bit elements
var e = [1.0, 2.5]                 // f64[2]
```

### Indexing

```vastnova
a[0] = 10        // indexed assignment
var x = a[0]     // indexed read
print(a[1])      // an index expression can appear anywhere an expression can
```

The index must be an integer. Indices are **not** bounds-checked; an out-of-range index reads or writes memory outside the array.

### Arrays in Functions

Function parameters and return types may use fixed-length array types. Arrays are passed and returned **by value**.

```vastnova
fn sum(values : i32[5]) -> i32 {
    var total = 0
    var i = 0
    while i < 5 {
        total = total + values[i]
        i = i + 1
    }
    return total
}

fn make_values() -> i32[3] {
    return [1, 2, 3]
}

var scores : i32[5] = [10, 20, 30, 40, 50]
print(sum(scores))            // 150
print(sum([1, 2, 3, 4, 5]))   // 150

var values = make_values()
print(values[0], values[1], values[2])   // 1 2 3
print(make_values()[1])                   // 2
```

- Parameter and return types must include the array length (`i32[5]`, not `i32[]`).
- An array variable passed as an argument must match the parameter type exactly; an array literal only needs the right number of elements and is converted to the parameter's element type.
- A function returns an array with `return`. The result can be stored in a `var`, indexed directly (`make_values()[0]`), passed to another array parameter, or returned again.
- Inside the function the parameter behaves like a local array: read or write elements with indexing. It cannot be printed or assigned as a whole.

### Restrictions

- Arrays must have a positive, integer, literal length.
- Nested arrays (`i32[2][3]`) are not supported.
- Arrays cannot be used as constants (`let`).
- An array cannot be used as a general whole value: `print(a)` and `a = [1, 2]` are errors. Read or write single elements instead. Arrays can be passed to and returned from functions, and a variable can be initialized from an array-returning call (see [Arrays in Functions](#arrays-in-functions)).
- An array literal can appear as the initializer of a `var` declaration, as an argument to an array parameter, or as a function return value (see [Arrays in Functions](#arrays-in-functions)), and it cannot be empty.

## 5. Arithmetic Expressions

Operators: `+` `-` `*` `/` with standard precedence (multiplication/division before addition/subtraction). Parentheses can override precedence.

A unary minus (`-`) can be applied to a number, a variable, or any parenthesized expression. Its precedence is higher than any binary operator.

```vastnova
var a = 10 + 5        // 15 (i32)
var b = 3.14 * 2      // 6.28 (f64)
var c = 20 / 3        // 6 (integer division)
var d = "Hello" + " " + "World"  // "Hello World" (str)

var e = -5            // -5
var f = -a            // -10
var g = -(3 + 4)      // -7
var h = -a * 2        // (-10) * 2 = -20
```

All arithmetic is **type-aware**:

- Two integers must have the same signedness. The result keeps that signedness and uses
  the wider of the two widths (`i3 + i18` → `i18`; `u8 + u8` → `u8`).
- Mixing signed and unsigned integers is a compile error. An untyped integer literal or
  constant adapts to the other operand's type when its value fits, so `u8 + 1` stays
  `u8`, while `u8 + 300` is an error (300 does not fit) and `i3 + 100` becomes `i32`.
- Integer arithmetic **wraps modulo 2^N**. Signed values use two's complement:
  `3 + 1` in `i3` is `-4`, and `7 + 1` in `u3` is `0`. There are no overflow traps.
- Integer division truncates toward zero for signed types and rounds down for unsigned
  types. Division by zero is not checked.
- An integer combined with a float produces `f64`. Two floats produce the wider float
  type (`f16 + f32` → `f32`, `f32 + f64` → `f64`, `f64 + f128` → `f128`).
- String `+` performs **concatenation** (only between strings).
- Mixing numbers and strings in an operator is a compile error; use `str()` to convert
  numbers explicitly.

```vastnova
var a : i3 = 3
print(a + 1)              // -4
var b : u3 = 7
print(b + 1)              // 0
var c : i18 = 100000
print(c + a)              // 100002
print(int(1, 32) + 0.5)   // 1.500000 (f64)
```

Narrowing a non-constant integer or changing its signedness requires an explicit
`int()` or `uint()` conversion (see [Conversion from strings](#conversion-from-strings-int-uint-float)).

## 6. Output: `print`

`print` takes any number of arguments (numbers, strings, variables) and prints them separated by spaces, ending with a newline. It automatically chooses the correct formatting.

```vastnova
print("Hello")               // prints: Hello
print(a, b, c)               // prints the values of a, b, c separated by spaces
print("Result:", a + b)      // mixed literals and expressions
```

`print` cannot print a whole array or a `void` value.

## 7. Input: `input`

`input` reads a line from standard input and returns it as a `str`. It can take an optional prompt argument (printed before reading).

```vastnova
var name = input("Enter your name: ")   // prints prompt, then reads a whole line
var age = input()                       // reads without prompt
print("Hello,", name, "age:", age)
```

The whole line is read, including spaces, up to the newline (or end of input). The buffer grows as needed, so line length is not limited. The prompt is printed as a plain string, so it may contain `%` safely.

`input` always returns a string. To perform numeric operations, convert the result using `int()`, `uint()`, or `float()`.

## 8. Conditionals: `if` / `else`

The `if` statement evaluates a condition and executes the `then` block if true; optionally an `else` block runs if false.

**Condition syntax:**  
Conditions can use comparison operators `>`, `<`, `>=`, `<=`, `==`, `!=` and logical operators `&&` (AND), `||` (OR). Parentheses are **not** required around the condition.

```vastnova
var score = int(input("Enter your score: "))
if score >= 90 {
    print("Excellent")
} else {
    if score >= 60 {
        print("Pass")
    } else {
        print("Fail")
    }
}
```

Both `then` and `else` blocks **must** be enclosed in `{ }`, even for a single statement. There is no `else if` shortcut; use nested `if` inside an `else` block instead.

`&&` and `||` **short-circuit**: the right operand is evaluated only when its value can affect the result.

A condition may be an integer, a float, or a string; zero and the empty string are false, and any other value is true.

## 9. Loops: `while`

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

`break` and `continue` can only appear inside a loop body; using them outside is a compile error.

## 10. Functions

Functions are declared with the `fn` keyword. Parameters must have explicit types. The return type is written after `->`; if omitted, the function returns nothing (`void`).

### Declaration

```vastnova
fn name(p1 : type1, p2 : type2) -> returnType {
    // body
}
```

### No parameters, no return value

```vastnova
fn greet() {
    print("Hello!")
}

greet()   // prints Hello!
```

### With parameters

```vastnova
fn greet(name : str) {
    print("Hello,", name)
}

greet("Alice")   // Hello, Alice
greet("Bob")     // Hello, Bob
```

Parameters and return types may also be fixed-length arrays; see [Arrays in Functions](#arrays-in-functions).

### With a return value

```vastnova
fn add(a : i32, b : i32) -> i32 {
    return a + b
}

var s = add(3, 4)
print("3 + 4 =", s)     // 7
```

### Mixing types

```vastnova
fn square(x : f64) -> f64 {
    return x * x
}

var r = square(2.5)
print(r)                // 6.250000
```

### Return rules

- In a non-`void` function, every path should end with `return expr`. The compiler warns and inserts a default return if a path is missing.
- In a `void` function, use `return` (without a value) or omit it entirely. Using `return expr` inside a `void` function is an error.
- The returned expression is automatically converted to the declared return type when possible (e.g., `i32` → `f64`).
- An array return value is copied and must match the declared array type exactly; array types are not converted.
- `return` outside a function (including at the top level of the program) is an error.

### Calling a function as an expression

A call that returns a value can appear anywhere an expression is allowed:

```vastnova
var x = add(1, 2)
print(add(x, 10))
```

A call that returns nothing can only be used as a statement:

```vastnova
greet("Alice")      // OK
var y = greet("Alice")  // ERROR: cannot assign void
```

### Rules and restrictions

- Parameter and return types must be explicit (return type may be omitted, meaning `void`).
- Array parameter and return types must have a fixed length (for example `i32[5]`).
- Function names must be unique; declaring `fn main` or duplicating a function name is an error.
- Functions may call each other before their definitions appear, so recursion works.

### Recursion

```vastnova
fn fib(n : i32) -> i32 {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}

print("fib(10) =", fib(10))   // 55
```

## 11. String Operations

### Escapes

String literals support these escape sequences: `\n` (newline), `\t` (tab), `\r` (carriage return), `\0` (NUL), `\\` (backslash), `\"` (quote). Any other `\x` sequence is kept literally as backslash plus `x`.

```vastnova
print("line one\nline two")
print("tab\there")
```

### String Indexing

A string can be indexed with an integer to read one byte as a new one-character string. Index `0` is the first byte, so for ASCII text it is the character at that position.

```vastnova
var a : str = "abc"
print(a[0])            // a
print(a[1])            // b
print(a[0] + a[2])     // ac
print("xyz"[2])        // z
```

Indices are not bounds-checked. Because strings are UTF-8, non-ASCII characters occupy multiple bytes, so indexing works on bytes rather than whole characters. A string index is a value, not an assignable location: `a[0] = "x"` is an error; build a new string with concatenation instead.

### Concatenation (`+`)
Two strings can be concatenated with `+`. The result is a newly allocated string.

```vastnova
var first = "Hello"
var last = "World"
var full = first + " " + last   // "Hello World"
```

### Comparison
Strings can be compared with `==` and `!=` only; ordering comparisons (`>`, `<`, `>=`, `<=`) are compile errors. Comparing a string with a number is also an error.

### Conversion from numbers (`str()`)
The built-in function `str(value)` converts an integer or float to its string representation, and returns a string unchanged. It works for every integer width, including widths above 64 bits.

```vastnova
var pi = 3.14159
var pi_str = str(pi)          // "3.14159"
var msg = "Pi is " + pi_str
print(msg)                   // Pi is 3.14159

var big : i128 = int(18446744073709551615, 128)
print(str(big))               // 18446744073709551615
```

### Conversion from strings (`int()`, `uint()`, `float()`)
These built-in functions convert a number or string to an integer or float. They are essential for processing user input numerically.

| Call | Result |
|------|--------|
| `int(x)` | signed integer, default width `i32` |
| `int(x, N)` | signed integer of width `N` |
| `uint(x)` | unsigned integer, default width `u32` |
| `uint(x, N)` | unsigned integer of width `N` |
| `float(x)` | float, default `f64` |
| `float(x, N)` | float of width `N` (`16`, `32`, `64`, or `128`) |

```vastnova
var age_str = input("Enter your age: ")
var age = int(age_str)          // i32
var small = int(300, 8)         // 44 (300 modulo 256)
var byte = uint(-1, 8)          // 255
var narrow = int(7, 3)          // -1
var weight = float("70.5")      // f64
var single = float(1.5, 32)     // f32
var quad = float(2.25, 128)     // f128
```

- Integer conversions truncate/wrap to the target width; signed and unsigned
  reinterpretation uses the target type's bit pattern.
- Float-to-integer conversion truncates toward zero.
- Integer-to-float conversion rounds to the nearest representable value.
- A string argument is parsed as a decimal number (`int`/`uint`) or a floating-point
  number (`float`). Integer parsing works at the requested width, including widths
  above 64 bits.
- Arrays cannot be converted.

### Implicit conversions

The compiler inserts these conversions automatically:

- an integer constant whose value fits the target integer type
- widening between integers of the same signedness (`i3` → `i18`, `u8` → `u64`)
- widening between floats (`f16` → `f32` → `f64` → `f128`)
- integer to float
- float to a wider float, and float narrowing (for example `f64` to `f32`), which may lose precision

Apart from constants whose value fits, everything else requires an explicit conversion:
integer narrowing, any signed/unsigned change, and float-to-integer. This applies to
variable initialization, assignment, function arguments, and `return` values.

```vastnova
var a : i3 = 2
var b : i18 = a          // OK: widening
var c : i64 = 42         // OK: constant fits
var d : i3 = int(100, 3) // OK: explicit, wraps

let k : u8 = 5
var m : i32 = k          // OK: let constant fits

var e : u8 = 5
var f : i32 = e          // ERROR: signedness change
var g : i32 = 5
var h : u8 = g           // ERROR: signedness change
var i : i8 = 1000        // ERROR: constant does not fit
var j : i32 = 3.5        // ERROR: use int(3.5)
```

## 12. Variable Reassignment

Variables declared with `var` can be reassigned at any time. The new value must be compatible with the variable's type; numbers and strings are not implicitly converted into each other.

```vastnova
var x = 10
x = 20              // OK
x = int(3.5)        // OK: truncates to 3
x = uint(-1, 8)     // ERROR: u8 is not i32
x = "text"          // ERROR: cannot convert a string to a number
```

Assignment uses the same implicit conversion rules as initialization (see
[Implicit conversions](#implicit-conversions)).

Constants (`let`) cannot be reassigned. A whole array cannot be reassigned; assign individual elements instead.

## 13. Modules and Imports

VastNova supports a simple module system via the `import` statement.

### Syntax
```vastnova
import "path/to/file.vn"
```

Paths are resolved relative to the directory of the file containing the `import` statement. If the file is not found there, the compiler falls back to the current working directory. Absolute paths are used as-is.

Duplicate imports are ignored – each file is loaded only once, and circular imports are safely handled.

The standard library lives under `std/` in the project root and is imported by path:

```vastnova
import "std/bool.vn"
```

### Example: Using Boolean Constants
`std/bool.vn`:
```vastnova
let true = 1
let false = 0
```

Program:
```vastnova
import "std/bool.vn"

var flag = true
if flag == true {
    print("It's true!")
}
```

## 14. Complete Example Program

Below is a full program that uses almost every feature:

```vastnova
// Input and output
var name = input("What is your name? ")
var age_str = input("How old are you? ")
var age = int(age_str)

// Arithmetic
var birth_year = 2025 - age
var next_age = age + 1

// Arrays
var scores : i32[3] = [90, 75, 60]
scores[0] = scores[0] + 5
var total = scores[0] + scores[1] + scores[2]
print("Total score:", total, "first:", scores[0])

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

// Function example
fn max(a : i32, b : i32) -> i32 {
    if a > b {
        return a
    } else {
        return b
    }
}

print("max(3, 7) =", max(3, 7))
print("max(-5, 2) =", max(-5, 2))
```

## 15. Errors and Limitations

- Integer widths are limited to 65535 bits. Arithmetic wraps modulo 2^N; overflow is not detected.
- There are no bitwise or shift operators, so very wide integers cannot be manipulated bit by bit.
- Only `f16`, `f32`, `f64`, and `f128` are available; other float widths such as `f9` or `f80` are compile errors.
- Arrays are fixed-length and one-dimensional; no resizing, no nested arrays, no bounds checking.
- Arrays can be function parameters and return values, but only by value with fixed-length types. They cannot be constants, printed, or assigned as a whole.
- No dictionaries, structs, or other composite types.
- No `else if` shortcut – nest `if` inside `else`.
- String comparison only supports `==` and `!=`; `>`, `<`, `>=`, `<=` on strings are compile errors.
- Strings can be indexed to read one byte as a one-character string; indices are not bounds-checked, and individual characters cannot be assigned.
- `break` and `continue` can only appear inside loops.
- Imported files are loaded at compile time; no dynamic imports.
- All parameters must have explicit types; return type is written after `->`.
- No overloading: function names must be unique.

## 16. Further Reading

For compiler internals, see the source code in the [`src/`](../src/) and [`include/`](../include/) directories.
