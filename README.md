[![Coded with Cursor](https://vibecoded.fyi/badges/flat/agents/cursor.svg)](https://vibecoded.fyi/)

# haxellvm

`haxellvm` is a small compiler from a deliberately useful subset of Haxe into
native object code. The compiler is C17 and creates its module through LLVM's C API
(`llvm-c/Core.h` and `llvm-c/Analysis.h`), then verifies and writes the module.

The compiler is built with `clang`, pinned in the Makefile with `override CC := clang`.
GCC cannot be selected through either the environment or `make CC=gcc`.

## Layout

```text
include/haxellvm/  Public compiler headers
src/                CLI, lexer, parser, and LLVM C API code generator
tests/              Compiler regression tests
examples/           Small hosted Haxe programs
haxeos/             Freestanding Limine kernel project
```

## Modules

Top-level `import module;` resolves `module.hx` relative to the importing source
file. Imported typedefs and constant helper functions are available through the
qualified module path. Modules may contain declarations without an entry point.

```haxe
import limine;

@:elfSection(".limine_requests")
static var revision:Array<Int> = limine.LimineBaseRevision(6);
```

Both `// line comments` and `/* block comments */` are accepted. Unterminated
block comments are a lexical error.

## Compiler Defines

`-D NAME` and `-D NAME=value` define a compiler variable. `#if NAME`, `#else`,
and `#end` select source branches before parsing; inactive lines retain their
line positions for diagnostics.

```haxe
#if debug
trace("debug build");
#else
trace("release build");
#end
```

## Supported Haxe

- `class Main` with `static function main()`
- `Int`, `Bool`, and string literals
- `var` declarations and assignment
- integer arithmetic and comparisons
- `if` / `else`, `while`, range `for`, `break`, `continue`, and `return`
- `trace(value)` for integers, booleans, and strings
- `asm("instruction")` for target-specific inline assembly with no operands

## Sized Integers

`IntSize<N>` and `UIntSize<N>` are haxellvm-specific integer annotations. They
lower to LLVM `iN` storage for any $1 \leq N \leq 65535$. `IntSize` sign-extends
when widened; `UIntSize` zero-extends.

```haxe
var status:UIntSize<8> = 255;
var offset:IntSize<16> = -2;
```

## Integer Operators

The freestanding integer backend supports `+`, `-`, `*`, `/`, `%`, `&`, `|`,
`^`, `<<`, `>>`, `>>>`, comparisons, and their compound-assignment forms.
`>>>` uses LLVM logical right shift; `>>` uses arithmetic right shift. Floating
point operations and managed-string concatenation/comparison need runtime and
type-system support and are not lowered yet.

## Fixed Arrays

`FixedArray<T, N>` is compiler-specific fixed storage, not Haxe's managed
`Array`. `T` must be `IntSize<M>` or `UIntSize<M>`, and `N` is a positive
compile-time length. It lowers directly to LLVM `[N x iM]` storage.

```haxe
var bytes:FixedArray<UIntSize<8>, 4> = [1, 2, 3, 4];
bytes[1] = 258; // narrowed to 2
trace(bytes[1]);
```

Unsized `Array<T>` in a typedef is an ABI pointer field, including when `T` is
another typedef. It is not fixed storage; use `FixedArray<T, N>` when the
layout requires an inline `[N x T]` array.

```haxe
typedef VideoMode = { var width:UIntSize<64>; };
typedef Framebuffer = { var modes:Array<VideoMode>; };
```

## Typedef Records

Haxe record typedefs lower to named LLVM structs. A typed variable may use a
record literal; named fields are matched case-insensitively and omitted fields
are zero-initialized.

```haxe
typedef Point = {
	var x:UIntSize<32>;
	var y:UIntSize<32>;
};

var point:Point = { x: 10, y: 20 };
```

The same literal form works for static globals, including globals placed with
`@:elfSection`.

Typedefs may also live in a standalone module with no class or entry point.
They still require their terminating semicolon:

```haxe
typedef Screen = {
	var width:UIntSize<64>;
};
```

## Pointers

`Ptr<T>` is compiler-specific typed pointer storage. `T` must be `IntSize<M>`
or `UIntSize<M>`, or a typedef name. Use `&` to take a local address and `*` to
read or write the pointee. An integer initializer creates a raw address with
LLVM `inttoptr`, for memory-mapped hardware.

```haxe
var value:IntSize<16> = -2;
var pointer:Ptr<IntSize<16>> = &value;
*pointer = -7;

var vga:Ptr<UIntSize<16>> = 0xB8000;

typedef Response = { var count:UIntSize<64>; };
typedef Request = { var response:Ptr<Response>; };
var request:Request = { response: null };
```

`Array<Ptr<T>>` is an ABI-owned sequence of pointers. Indexing loads a pointer
element, so its pointee fields are available directly:

```haxe
var framebuffer = response.framebuffers[0];
var width = framebuffer.width;
```

String literals decode standard escapes plus octal (`\033`), hexadecimal
(`\x1b`), fixed Unicode (`\u001b`), and braced Unicode (`\u{1b}`).

This is a focused compiler, not a replacement for the complete Haxe language:
classes beyond `Main`, methods, arrays, objects, imports, and standard-library
calls are intentionally outside its current grammar.

## Inline Assembly

`asm("template")` emits target-native inline assembly with no operands. For
operands, use LLVM's constraint syntax explicitly:

```haxe
asm("cmp $0, $1", "r,r,~{flags}", left, right);
asm("hlt");
```

`$0`, `$1`, and so on identify operands. Constraints select target registers,
memory operands, and clobbers, so the same syntax can express x86 and ARM
assembly. A first output constraint stores LLVM's result back to its matching
local variable:

```haxe
asm("in $0, $1", "=r,{dx}", value, port);
asm("out $0, $1", "{dx},{al}", port, value);
```
## ELF Metadata

`@:entryPoint` is required to select an emitted entry function and preserves
its raw method name for linker scripts. `@:retainFunctionName` remains available
for non-entry functions as function lowering expands. `@:elfSection("name")`
places a static global in the requested ELF section. Constant helper functions
are evaluated while lowering globals, so a Limine-style helper and global may
share a Haxe source name: only the global becomes an ELF symbol.

`@:mutable` makes a static global writable. Combine it with `@:elfSection` for
objects a bootloader updates, such as a Limine base-revision request or a
response-bearing request struct.

`@:extern` declares a function implemented outside the current Haxe module.
It emits an LLVM declaration but no function body.

```haxe
@:extern
static function hardware_ready():Bool;
```

```haxe
static function limine_base_revision(revision:Int):Array<Int> {
	return [0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, revision];
}

@:elfSection(".limine_requests")
@:mutable
static var limine_base_revision:Array<Int> = limine_base_revision(6);

@:entryPoint
static function kmain():Void { asm("hlt"); }
```

## Run

```sh
make
./haxellvm examples/countdown.hx -o countdown.o
clang countdown.o -o countdown
./countdown

# LLVM IR is explicit.
./haxellvm --emit=llvm examples/countdown.hx -o countdown.ll
lli countdown.ll
clang countdown.ll -o countdown
./countdown

# Native assembly for the host target. x86 targets use Intel syntax by default.
./haxellvm --emit=asm haxe.hx -o haxe.s

# Native object file for the host target.
./haxellvm --emit=obj examples/countdown.hx -o countdown.o
clang countdown.o -o countdown
```

`--emit=llvm`, `--emit=asm`, and `--emit=obj` may appear anywhere in the
command. Object output is the default.

Pass LLVM CPU feature overrides with `-Xcpu FEATURES`. For an x86 kernel that
does not enable floating point or SIMD state, use:

```sh
./haxellvm -Xcpu -x87,-mmx,-sse,-sse2,+soft-float src/main.hx -o main.o
```

For target-specific assembly with operands, pass the LLVM constraint string as
the second argument followed by ordinary Haxe expressions.

## Test

```sh
make test
```