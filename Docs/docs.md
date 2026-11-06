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

VastNova has five primitive types and fixed-length arrays:

| Type  | Description                    | Example literals |
|-------|--------------------------------|------------------|
| `i32` | 32-bit signed integer (default) | `42`, `-5`       |
| `i64` | 64-bit signed integer          | `5000000000`     |
| `f32` | 32-bit IEEE-754 float          | `3.14`           |
| `f64` | 64-bit IEEE-754 float (default float) | `3.14`, `-0.001` |
| `str` | UTF-8 string (heap-allocated)  | `"Hello"`        |

All types are **static** but can be **inferred** when a value is given.

Integer literals that fit in 32 bits are `i32`; larger literals are `i64`. Literals containing a `.` are `f64`.

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

When the type is omitted, it is inferred from the literal:

- all integer elements that fit in 32 bits → `i32[n]`
- any element outside 32-bit range → `i64[n]`
- any floating-point element → `f64[n]`
- all string elements → `str[n]`
- numeric elements are widened to a common type (`i32` + `i64` → `i64`, integer + float → `f64`)
- mixing strings and numbers in one literal is an error

### Indexing

```vastnova
a[0] = 10        // indexed assignment
var x = a[0]     // indexed read
print(a[1])      // an index expression can appear anywhere an expression can
```

The index must be an integer. Indices are **not** bounds-checked; an out-of-range index reads or writes memory outside the array.

### Restrictions

- Arrays must have a positive, integer, literal length.
- Nested arrays (`i32[2][3]`) are not supported.
- Arrays cannot be used as parameter types, return types, or constants (`let`).
- An array cannot be used as a whole value: `print(a)` and `a = [1, 2]` are errors. Read or write single elements instead.
- An array literal can only appear as the initializer of a `var` declaration, and it cannot be empty.

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
- If both operands are integers, the result is an integer (truncating division). If either operand is `i64`, the result is `i64`; otherwise `i32`.
- If either operand is a float, the result is `f64`.
- String `+` performs **concatenation** (only between strings).
- Mixing numbers and strings in an operator is a compile error; use `str()` to convert numbers explicitly.

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

`input` always returns a string. To perform numeric operations, convert the result using `int()` or `float()`.

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
- Arrays cannot be parameter or return types.
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
The built-in function `str(value)` converts an integer or float to its string representation, and returns a string unchanged.

```vastnova
var pi = 3.14159
var pi_str = str(pi)          // "3.14159"
var msg = "Pi is " + pi_str
print(msg)                   // Pi is 3.14159
```

### Conversion from strings (`int()`, `float()`)
These built-in functions convert a string (or number) to an integer or float. They are essential for processing user input numerically.

```vastnova
var age_str = input("Enter your age: ")
var age = int(age_str)        // convert to i32
var weight_str = input("Enter your weight: ")
var weight = float(weight_str) // convert to f64
print("Next year you will be", age + 1)
```

If the argument is already a number, `int()` and `float()` simply cast it to the desired type. Arrays cannot be converted.

## 12. Variable Reassignment

Variables declared with `var` can be reassigned at any time. The new value must be compatible with the variable's type; numbers and strings are not implicitly converted into each other.

```vastnova
var x = 10
x = 20          // OK
x = 3.5         // OK: truncated to 3 when stored in i32
x = "text"      // ERROR: cannot convert a string to a number
```

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

- Arrays are fixed-length and one-dimensional; no resizing, no nested arrays, no bounds checking.
- Arrays cannot be function parameters, return values, or constants, and cannot be printed or assigned as a whole.
- No dictionaries, structs, or other composite types.
- No `else if` shortcut – nest `if` inside `else`.
- String comparison only supports `==` and `!=`; `>`, `<`, `>=`, `<=` on strings are compile errors.
- `break` and `continue` can only appear inside loops.
- Imported files are loaded at compile time; no dynamic imports.
- All parameters must have explicit types; return type is written after `->`.
- No overloading: function names must be unique.

## 16. Further Reading

For compiler internals, see the source code in the [`src/`](../src/) and [`include/`](../include/) directories.
