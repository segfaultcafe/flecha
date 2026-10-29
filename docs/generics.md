# Generics

Flecha implements compile-time specialization of functions, structures, and
callable signatures. Type parameters are ordinary identifiers declared between
`<` and `>`; `T` is not a keyword or a wildcard type. Generic types and callable
aliases use explicit concrete type arguments. Free-function calls may infer them
from arguments or an explicit expected result type.

```flecha
T identity<T>(T value) { return value; }
(A, B) pair<A, B>(A first, B second) { return (first, second); }

structure Box<T> { T value; };
T Box<T>::get(&const self) { return self->value; }
void Box<T>::set(&self, T value) { self->value = value; }

func Mapper<T>(T value) => (T);

i32 main() {
    box := Box<i32>::init();
    Mapper<i32> mapper = identity<i32>;
    box.set(identity(42));
    return box.get();
}
```

Explicit specialization works both at a call (`identity<i32>(42)`) and when a
generic function is retained as a callable value (`mapper = identity<i32>`).
At a direct free-function call, type arguments may be omitted when every type
parameter has one consistent inference:

```flecha
T identity<T>(T value) { return value; }
T parse<T>(stringv value) { return none; }

i32 copied = identity(42);       // T is i32 from the argument.
i32 parsed = parse("42");        // T is i32 from the declared result type.
```

Supplied arguments and an available expected result type are combined. If they
disagree, or any type parameter remains unknown, the call is rejected and must
use explicit arguments. Omitted default arguments provide no inference evidence.
An inferred declaration such as `value := parse("42")` has no expected result
type and therefore cannot infer `T`. Numeric and floating literals use their
ordinary default types, `i32` and `f64`, when they are the evidence.

Inference applies only to direct free-function calls. Generic function values,
generic types, callable aliases, and generic methods keep explicit arguments.
Type-set constraints are checked after inference exactly as for an explicit
specialization.

A method repeats its owner's parameter names in the same order. It may declare
additional parameters: `U Box<T>::convert<U>(&const self)`. Such a method is called
as `box.convert<i32>()`. Lifecycle methods cannot add type parameters of their
own. Method declarations remain outside the structure body.

## Callable signatures

A generic callable alias specializes to the same structural callable type that
could be written as a nongeneric alias. Substitution applies to parameter and
result types, including nested types, and preserves `unsafe`, `own`, `lend`, and
borrowed-result contracts.

```flecha
func Mapper<T>(T value) => (T);
func Fold<T, U>([]const T values, U initial) => (U);
func Borrower<T>(&T value) => (&T) <- value;

Mapper<i32> increment = (value) => { return value + 1; };
```

Specialized callable aliases work in function parameters and results, aggregate
fields and containers, imported module APIs, and other generic declarations.
They also provide the expected type for inferred anonymous functions. Callable
aliases are structural, so independently declared aliases with identical
specialized signatures are compatible. Type arguments are never inferred for
the alias itself: missing, extra, and unresolved arguments are diagnosed.

## Types and references

Each generic declaration and ordered argument list identifies one specialization.
`Box<i32>` and `Box<f64>` are different types; repeated `Box<i32>` uses share a
type, including across imports. Nested applications such as `Box<Box<i32>>` work.
Concrete type arguments can include pointers, arrays, slices, tuples, and named
types, subject to the ordinary rules at each substituted use.

Generic functions can take `&T` and `&const T` parameters. Reference storage rules
are unchanged: a structure field cannot store a reference, so a `Box<&const i32>`
whose field is `T` is invalid. References take neither `own` nor `lend`.
Raw pointers alone still carry no ownership contract; generic `own *T` and
`lend *T` declarations undergo the existing ownership analysis.

`const` controls mutation. It does not make a resource copyable. Structures with
unsafe `tear` remain immovable and noncopyable, and `tear` remains callable only
by the compiler. Instantiation does not bypass borrowing, allocation checking,
unsafe permissions, visibility, or lifecycle rules.

## Checking and modules

Templates are parsed when loaded. Parameter-name collisions, malformed generic
declarations, and private types exposed by exported generic signatures are
rejected even without an instantiation. Concrete signatures and bodies are
checked when instantiated. Operations must be valid for the supplied types. A
type parameter may additionally name a closed [type-set
constraint](type_sets.md), such as `T: integer`. The constraint is checked
before specialization, and ordinary body checking still follows. Type sets
restrict identity; they do not grant operations or provide dynamic dispatch.
Instantiating a structure also checks its ordinary methods. Methods with
additional type parameters are checked when explicitly instantiated on their
actual receiver.

Export and import rules match ordinary declarations. A specialization resolves
helpers in the defining module, while argument types retain their identity from
the caller's module. Importing a structure includes its public methods and
preserves private-member restrictions.

Ordinary recursive calls to the same specialization are supported. Expanding
recursion is bounded: specialization permits at most 64 levels of instantiation
ancestry, 2,048 generated declarations, and 1,000,000 generated tokens per
compilation. Exceeding a budget is an error, not permission to emit unchecked
code. Invalid generic applications use `FLECHA-S0054`; errors in instantiated
code include instantiation context.

In expressions, a syntactically valid `<types>` followed by `(` or, for a type
name, `::` is a type application. Parenthesize comparisons when their token
sequence would otherwise have that form. Ordinary field comparisons such as
`box.value < limit` remain comparisons.

## Compiler implementation and scope

The parser retains template tokens and identifies type parameter substitution
sites through the AST. A pre-semantic expansion pass materializes concrete
AST declarations, caches specializations, and preserves source/module identity.
The normal semantic, allocation, lowering, and native code generation passes
then process those declarations. Generic templates themselves emit no code.

There is no partial explicit/inferred type-argument list, default type argument,
variadic generic parameter pack, constant generic parameter, user specialization
override, trait system, or separate compiled generic interface yet. The
language's untyped forwarding packs are a separate compile-time call-forwarding
facility and cannot participate in generic substitution. The standard
library provides [`RawArray<T>`](dynamic_arrays.md), implemented in Flecha. Its
runtime storage currently supports numeric, boolean, and character elements;
pointers and structures are not yet supported. Element restrictions are
enforced when the relevant generic operations are instantiated.
