# Nondescript

A simple, embeddable scripting language for C applications.
Nondescript occupies a space similar to Lua: a small, single-file implementation
that can be compiled into any C project with minimal fuss.

## Features

- **Single-file implementation** — one `.c` and one `.h`, no dependencies beyond the C standard library
- **C11 compatible** (really C99 + `_Alignof`)
- **AppleScript-style syntax** — natural-language-inspired, case-insensitive
- **Extensible grammar** — register host commands with custom keyword patterns
- **Chunk accessors** — `word 1 of myString`, `item 3 of myList`, etc. You can register your own chunk types at runtime
- **`given` blocks** — pass a block of code to a function
- **List comprehensions** - `set nonSpaces to every character of myString where it != ' '`
- **Pluggable allocators** — pool and limit allocators included
- **Slot-based C API** - to (hopefully) make it hard to break GC
- **A distinct 90s feel** - for a bit of nostalgia

## Building

Nondescript is designed to be compiled directly into your project:

```c
#include "nondescript.h"
```

```sh
cc -o myapp myapp.c nondescript.c -lm
```

## Quick Start

```c
#include <stdio.h>
#include "nondescript.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: nds <script.nds>\n");
        return 1;
    }

    NDSConfig config = {0};
    NDSContext *ctx = NDSContext_new(&config);
    NDSStatus status = NDSContext_evaluateFile(ctx, argv[1]);
    if (status != NDSStatus_OK)
        fprintf(stderr, "%s\n", NDSContext_getError(ctx));
    NDSContext_free(ctx);
    return status == NDSStatus_OK ? 0 : 1;
}
```

```
-- Functions
function greet(name)
    println("hello, ", name, "!")
end function

greet("world")

-- Chunks and set expressions.
set firstItem to item 1 of ["foo", "bar", "baz"]
println("firstItem:", firstItem)

-- List comprehensions.
set nonSpaces to every character of "foo bar      baz" where it != ' '
println("nonSpaces:", nonSpaces)

-- Blocks.
function map(input)
    my result is []
    
    for each item i in input do
        append given(i) to result
    end for
    
    return result
end function

set plus2 to map([1, 2, 3]) given (i)
    return i + 2
end given

println("plus2:", plus2)

-- Block scope. Set will set the innermost binding of that variable.
-- Use "my" to claim an inner binding for yourself. All structures
-- introduce new scope.
function functionScope()
    my outer is "outer"
    if true then
        my outer
        set outer to "inner now!"
        println("inside if block:", outer)
    end if
    println("outside if block:", outer)
end function

functionScope()

-- Error handling is per-function.
function errorFunction()
    my NaN is 1 / 0
on error myError
    println("errored:", myError)
end function

errorFunction()

-- Errors bubble up
function innerError()
    raise nothing message "foo"
end function

function outerError()
    innerError()
on error myError
    println("got error with message", message of myError)
end function

outerError()
```

## Examples

See the `examples/` directory:

- **calc** — Defines functions in script, calls them from C via `NDSContext_callFunction`
- **turtle** — Registers natural-language host commands (`move forward 100 steps`, `turn right 90 degrees`), runs a script, and writes SVG output

Build an example:

```sh
cd examples
c11 -o turtle turtle.c ../nondescript.c
./turtle
```

## License

LGPL

## TODO

- The whole bifurcation of bytes/characters is weird...should probably just make separate types.
- Tons of little fixes and cleanups. NDSValue/NDSObject split is ugly...
- There's basically zero documentation.
