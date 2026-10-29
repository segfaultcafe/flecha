# Definite assignment

Typed value declarations may omit their initializer only when the resolved type
is trivially zeroable. Omission is exactly shorthand for `= none`:

```flecha
i32 answer;
*u8 pointer;
string message = none;

answer = 42;
message = "ready";
```

`answer` is immediately readable as zero and `pointer` as null. A declaration
whose default construction performs work must spell `= none` explicitly.

The shorthand supports integer and floating-point types, `bool`, `char`, raw
pointers, and compound types whose elements have trivial zero values. It also
supports enums whose default is represented by zero and structures whose
canonical `zero` construction is trivial. Owning `string`, error values,
borrowed `stringv`, nonzero-default enums, and structures with nontrivial
`zero` construction require an explicit initializer. `void` is never a
variable type.

Inferred declarations continue to require an initializer:

```flecha
answer := 42;
```

## Reachable control flow

Analysis runs after name binding and uses resolved symbols, so shadowed names
remain distinct. Each function gets a control-flow graph with reads, writes,
declarations, conditions, and explicit transfer edges. Parameters start
initialized, and every accepted local declaration initializes its value. The
remaining definite-assignment checks primarily prevent jumps from bypassing a
declaration or initializer before a later read.

The same rule applies to `break`, `proceed`, and `jumpto`. Unreachable reads do
not produce `FLECHA-S0021`; name resolution and type checking still apply to
unreachable source. Diagnostics identify the exact read expression and are
reported once, after convergence, in source order within each function.

## Loops and conditional also clauses

`proceed` targets the innermost loop's condition, skipping its remaining body
and `also` clause. `break` targets that loop's exit. Normal body completion
reaches the conditional `also` clause, if present, before the next iteration.
Nested loops maintain separate targets. Assignments in a condition are modeled
in evaluation order. Boolean `and` and `or`
short-circuit: their right operands run only on the corresponding condition
edge. Integer bitwise operations evaluate both operands.

Conditions conservatively retain both outcomes, even for literals. This pass
does not perform constant folding or correlate repeated tests of a value.

## Positions, jumps, and scopes

Forward and backward `jumpto` edges connect directly to their resolved
`#position`, including positions in other nested loops. Positions join both
fallthrough and all reachable jump predecessors. Skipping a declaration or
assignment never initializes its variable:

```flecha
void rejected(bool run) {
    while run {
        jumpto use;
        i32 value = 42;
        #use
        stdoutln(value); // FLECHA-S0021 at value
        break;
    }
}
```

Leaving a scope removes its locals from initialization state. Jumping back
into that scope cannot reuse state from an earlier activation. A jump into a
scope may be accepted when all subsequent reads are preceded by assignments;
normal lexical visibility still governs which symbols can be named.

Owning strings have null storage at function entry, making cleanup safe even
when a jump skips their declarations. Cleanup emission includes all slots in
the scopes being exited, including later declarations and temporaries that
may have executed before a backward jump to an earlier exit. Transfers of
ownership clear the source storage so repeated execution cannot double-free
an allocation. This does not change the requirement that owning strings and
`stringv` declarations spell their initializers explicitly or extend
borrow-origin analysis.

## Verification

`make test` includes semantic, lowering, LLVM verification, and native
control-flow regressions. `make test-sanitizers` also runs the native
control-flow fixture with an instrumented runtime and leak detection enabled.
`make native-definite-assignment` preserves the original example's output and
exit status of 42.

## Shared control-flow representation

Graph construction now lives in `semantic/control_flow_graph.cpp`. The same
reachable edges and lexical scope metadata feed definite assignment and
[reference borrow checking](checked_references.md). Definite assignment uses
ordered name-read/reset/write nodes and its existing must-analysis. Reference
checking additionally uses expression/declaration actions, backward liveness,
and a forward may-analysis of origins and moved handles.
