# Semantic analysis

## Pipeline

Flecha processes source in three stages: the tokenizer produces ranged tokens,
the parser builds the move-only syntax tree, and `SemanticAnalyzer` binds and
checks that tree. Syntax types (`core::TypeNode` and `core::PrimitiveType`) stay
in the AST. `semantic::SemanticType` is the resolved type used by symbols and
expression checking.

Analysis first registers named error families, then every top-level function,
so signatures and calls may refer to declarations later in the file. It analyzes each function body in
source order. The resulting `SemanticModel` provides:

- the symbol declared by a function, parameter, or local declaration;
- the symbol resolved for each `NameExpression`;
- the resolved type of each expression;
- the compilation-unit, function, and block scopes created during analysis.

## Ownership and lifetimes

The AST exclusively owns its children with `std::unique_ptr`. It remains alive
while its semantic model is queried. A `SemanticModel` exclusively owns all
symbols and scopes. Scope tables and AST binding tables contain non-owning
pointers to those model-owned objects. Symbols and scopes are individually
heap-allocated, so moving a model does not invalidate those pointers. The
model never owns AST nodes and must not outlive the AST when its AST-keyed
queries are used.

## Scopes and declarations

The compilation-unit scope contains functions and named error types. Each function has a child scope
containing its parameters and top-level body locals. Conditional bodies,
`else` bodies, and `also` bodies introduce nested lexical scopes. Lookup starts
in the current scope and walks outward. A declaration name may appear only once
in a scope; a nested declaration may intentionally shadow an outer symbol.

Function declarations are callable and not writable. Parameters and local
variables are writable under the current language rules. A `:=` declaration
always creates a local in the current scope; it never updates an outer symbol.
Both explicit and inferred declarations are registered after their initializer
is analyzed, so a declaration cannot refer to itself and use-before-declaration
is diagnosed.

## Types and inference

Semantic types cover every syntax primitive plus the literal-only `null` and
`none` types, named error families, built-in `error`, the internal `function`
type, and a separate error-recovery type. Named error types preserve nominal
identity and implicitly widen to `error`; see [error_values.md](error_values.md).
Literal defaults are:

| Literal | Inferred type |
| --- | --- |
| Integer | `i32` |
| Floating point | `f64` |
| `true`, `false` | `bool` |
| Character | `char` |
| String | `stringv` |
| `null` | `null` |
| `none` | `none` |

Parentheses and valid unary, binary, assignment, name, and call expressions
propagate their resolved types. Distinct numeric types are not implicitly
coerced. Integer literals may adopt an expected integer type with a range check;
unconstrained literals retain the table defaults. `none` is contextually
assignable to non-void, non-reference value types, matching
the language specification's universal-zero rule. Arithmetic requires equal
numeric operand types; comparisons produce `bool`; logical/legacy bitwise
operators require equal boolean or integer types. Symbolic integer operators
`&` and `|` require equal integer operands, while `~` requires one integer
operand. Shifts preserve the left integer type and require a `usize` count.
Recursive compound types preserve
qualifiers at each layer. [Raw pointers](unsafe_pointers.md) require unsafe
permission for dereference; [checked references](checked_references.md) use
safe dereference and a separate borrow/move pass on the shared control-flow graph.

Floating-point type properties `nan`, `infinity`, `negative_infinity`,
`min_value`, `max_value`, `min_positive`, and `epsilon` resolve contextually for
`f32`, `f64`, and specialized `floating_point` parameters. Floating equality
uses IEEE ordered equality and unordered inequality: NaN never equals a value,
while `!=` is true when either operand is NaN. Relational comparisons remain
ordered and therefore return false for NaN operands.

Calls currently require a directly resolved function symbol. Analysis checks
callability, argument count, and each known argument type. Function values and
overloads are not yet part of the language model.

## Diagnostics and recovery

Semantic diagnostics use stable codes `FLECHA-S0001` through
`FLECHA-S0036`. The table below lists the initial binding/type codes; later
feature documents describe their additional codes. Declaration-name diagnostics use the exact identifier token
range retained by the AST; expression diagnostics use exact half-open AST
ranges. The analyzer returns an error type after a failed resolution or type
check so dependent checks suppress redundant cascades while independent
statements continue to produce useful diagnostics.

| Code | Meaning |
| --- | --- |
| `FLECHA-S0001` | Duplicate declaration in one scope |
| `FLECHA-S0002` | Unknown name |
| `FLECHA-S0003` | Incompatible declaration or assignment type |
| `FLECHA-S0004` | Invalid semantic assignment target |
| `FLECHA-S0005` | Resolved symbol is not writable |
| `FLECHA-S0006` | Callee is not callable |
| `FLECHA-S0007` | Wrong argument count |
| `FLECHA-S0008` | Incompatible call argument |
| `FLECHA-S0009` | Invalid unary or binary operand types |
| `FLECHA-S0010` | Non-boolean condition |
| `FLECHA-S0011` | Invalid return type |
| `FLECHA-S0012` | Inferred declaration has no usable value type |
| `FLECHA-S0013` | Invalid variable type, currently `void` |

Parser diagnostics remain separate. In particular, `FLECHA-P0003` preserves
the parser's invalid-assignment-target report, and semantic analysis adds
`FLECHA-S0004` when the recovered AST is analyzed.

## Fixed arrays

Array types retain their element type and literal length. Array initializer
elements receive contextual types; indexing resolves to a writable place only
when its receiver is addressable array storage. Const propagates through nested
array storage. The shared flow graph traverses list elements in order and repeat
initializers once; element stores never establish whole-array initialization.
Borrow analysis maps element places to the root array's owner. See
[fixed arrays](fixed_arrays.md) for the executable contract.

### Range and counted-loop analysis

Range bindings use Go-style index/value order, with inferred `usize` indices.
The source must be a fixed array; explicit index types must represent every
possible index and element bindings follow ordinary assignment compatibility.
Range source snapshots and iteration declarations use the shared scope,
initialization, and borrow analysis. The CFG routes `proceed` through counted-loop
updates, and header variables end at the loop boundary. Integer postfix increment
requires initialized writable storage and participates in borrow checking as a
write. See [iteration.md](iteration.md) for syntax and exact evaluation rules.

### Checked slices

`[]T` and `[]const T` borrow array storage or another slice. The checker tracks
slice handles using the same origins, moves, reborrow parents, last uses, and
scope constraints as references. Slice range sources receive shared reborrows.
`FLECHA-S0037` reports statically invalid slice bounds; dynamic bounds trap in
native code. See [checked_slices.md](checked_slices.md).

### Single-use callables

`once func` is structurally distinct from `func`. A compatible reusable
callable is assignable to a once destination, but the reverse conversion is
not. Every indirect call through a once signature records a move of the callee,
so definite assignment reports a second invocation or later use with
`FLECHA-S0027`. Inside a once anonymous function, an `own` capture may be moved,
returned through an owning contract, passed to an owning parameter, or freed;
the same operations remain invalid in reusable closures.

### Deferred calls

`defer` accepts direct functions, callable values, and resolved structure or
enum methods (`FLECHA-S0038` otherwise). Callee, receiver, and argument typing,
definite assignment, moves, and unsafe permission apply at registration.
Synthetic capture symbols keep callable borrow origins and receiver,
reference, or slice loans live until their enclosing scope exits, including
across backward jumps and control-flow joins. See [defer.md](defer.md) for
capture and cleanup rules.

### Tuples and destructuring

Tuple element types are structural and recursive. Expected tuple types supply
element contexts for literals. Destructuring resolves existing destinations
before analyzing its right-hand side and introduces new bindings afterward.
For `:=`, existing writable names in the current lexical scope are assignments;
other non-blank names are declarations, and at least one declaration is
required.
The shared CFG evaluates the complete source before destination writes, with
scope resets for declarations. Borrow checking treats each destination as a
write to its owner. `FLECHA-S0039` and `FLECHA-S0040` diagnose unsupported tuple
contents/layout and invalid destructuring. See [tuples.md](tuples.md).

## Structures

Nominal structure descriptors are registered before field types and methods,
allowing forward references while rejecting recursive inline layouts. Field and
method bindings preserve owner identity for privacy and receiver checking.
Generated init functions share the regular semantic, CFG, lowering, and codegen
pipeline. Build bodies participate in definite-assignment and borrow analysis;
construction-field references cannot escape the build block.

`FLECHA-S0043` diagnoses invalid structures/construction, `FLECHA-S0044` private
access, `FLECHA-S0045` lifecycle misuse, and `FLECHA-S0046` prohibited copies or
implicit moves. Existing unsafe, qualifier, and borrow diagnostics cover member
access and calls. See [structures.md](structures.md) for the language contract.
