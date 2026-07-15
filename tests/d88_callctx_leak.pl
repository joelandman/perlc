#!/usr/bin/perl
# In-depth test suite for D88: callCtx_ (the compiler's list-context-
# propagation flag) leaked list context through scalar-forcing operators
# (eq, ==, +, ., cmp, etc.) into a call buried inside one of their
# operands, at every call site that sets callCtx_ EXCEPT print/printf
# (which D12 already fixed narrowly, via CodeGen::isCallLikeForContext).
#
# Root cause: callCtx_ is a blunt, unscoped, one-shot flag — set to 1
# right before evaluating an entire RHS/argument expression tree at
# several call sites in codegen.cpp (list assignment RHS, lvalue slice
# RHS, hash-from-list RHS, etc.), then read and immediately reset by
# whichever Call/MethodCall/CallCodeRef node's own codegen happens to be
# reached next — with no regard for how deeply that call is nested inside
# other operators. `my @a = (ctx() eq "x")` set callCtx_=1 for the whole
# `(ctx() eq "x")` expression (since `my @a = ...` is a list-context
# assignment), and since nothing reset it before evaluating `eq`'s own
# operands, `ctx()` (buried inside `eq`) incorrectly inherited list
# context — even though real Perl's `eq` (like every other comparison/
# arithmetic/string/bitwise operator) always evaluates both of its own
# operands in scalar context, regardless of the surrounding expression's
# context.
#
# Fix: every non-short-circuit binary operator (arithmetic, string,
# comparison, bitwise, `x`, `<=>`/`cmp` — NOT `&&`/`||`/`//`/`?:`, which
# have their own separate branches earlier in emitBinOp and are handled
# differently, since e.g. `||`'s right-hand branch, if reached, actually
# should transparently inherit the outer context in real Perl) funnels
# through one single, shared spot in CodeGen::emitBinOp to evaluate its
# left/right operands. Saving callCtx_, clearing it to 0 before
# evaluating each operand, and restoring it afterward at that one choke
# point fixes the leak for this entire class of operators in one place —
# without needing to individually audit or special-case every call site
# elsewhere in the file that happens to set callCtx_=1.
#
# Several separate, unrelated, pre-existing bugs were found (and worked
# around, not fixed — logged as new defects) while writing this test:
#   D92 - a named `sub` declared inside a bare block, combined with
#         another named sub declared inside a DIFFERENT bare block in the
#         same file, a shared file-scope array, and a `check()`-style
#         helper sub, crashes at runtime (perl_clone() on a garbage
#         pointer) - reproduces identically with and without this D88
#         fix applied. Worked around here by declaring every sub at the
#         top level instead of nested inside a bare block (also better
#         style regardless).
#   D93 - `sub f { return wantarray() ? (LIST) : SCALAR; }` (the
#         idiomatic wantarray-dispatch pattern using a ternary) does not
#         correctly return the LIST branch's list in list context when
#         captured via `my @a = (f());` - silently gives an empty/wrong
#         result. Reproduces identically with and without this fix.
#         Worked around by using `if (wantarray()) { return (...) } else
#         { return ... }` instead of the ternary form throughout.
#   D94 - `||`'s right-hand operand, when reached (left operand falsy),
#         does not correctly inherit the outer expression's list context
#         - `my @a = (falsy() || listret())` gives listret() scalar
#         context instead of the list context real Perl would give it.
#         Reproduces identically with and without this fix (this fix
#         does not touch `||`'s branch in emitBinOp at all). Not
#         exercised in this test suite for that reason.
#   D95 - `EXPR x N` used as an array-assignment RHS (e.g. `my @a = (f()
#         x 2)`) is always treated as list repetition by emitArrayPtr's
#         "(LIST) x N in array context" special case, even when the left
#         operand isn't actually a parenthesized list literal at all
#         (real Perl only does list-repetition when `x`'s left operand is
#         syntactically `(...)`-enclosed; a bare `f() x 2` is always
#         scalar/string repetition regardless of the assignment's own
#         context) — giving the wrong element count. Reproduces
#         identically with and without this fix (this fix never touches
#         emitArrayPtr). Worked around in this test by using plain scalar
#         assignment for the `x` operator check instead.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @log;
sub ctx     { push @log, wantarray() ? "list" : "scalar"; return "x"; }
sub numctx  { push @log, wantarray() ? "list" : "scalar"; return 5; }
sub pctx    { push @log, wantarray() ? "list" : "scalar"; return "z"; }
sub listret { if (wantarray()) { return (1,2,3); } else { return "scalar"; } }

# ── Section 1: the original repro — `eq` inside a list-assignment RHS ────
{
    @log = ();
    my @a = (ctx() eq "x");
    check('original_repro_eq', "@log" eq "scalar");
    check('original_repro_eq_result', "@a" eq "1");
}

# ── Section 2: every other non-short-circuit operator class ──────────────
{
    @log = ();
    my @a = (numctx() == 5);
    check('numeric_eq_scalar', "@log" eq "scalar");
}
{
    @log = ();
    my @a = (numctx() + 1);
    check('arithmetic_plus_scalar', "@log" eq "scalar");
}
{
    @log = ();
    my @a = (ctx() . "y");
    check('string_concat_scalar', "@log" eq "scalar");
}
{
    @log = ();
    my @a = (ctx() cmp "x");
    check('cmp_spaceship_scalar', "@log" eq "scalar");
}
{
    @log = ();
    my @a = (numctx() <=> 3);
    check('numeric_spaceship_scalar', "@log" eq "scalar");
}
{
    # NOT tested in array-assignment position: `my @a = (ctx() x 2)` hits
    # a separate, unrelated, pre-existing bug found while writing this
    # test (logged as D95, not fixed here) — emitArrayPtr's "(LIST) x N
    # in array context" special case wrongly triggers for ANY `x` BinOp
    # reached while evaluating an array-context RHS, even when the left
    # operand isn't actually a parenthesized list literal (real Perl only
    # does list-repetition when `x`'s left operand is syntactically a
    # `(...)`-enclosed list; `ctx() x 2` alone is always scalar/string
    # repetition regardless of the assignment's own context) — giving
    # `@a` 2 elements instead of the correct 1. Using plain scalar
    # assignment instead sidesteps that unrelated bug and still exercises
    # this fix's actual concern (`x`'s own operand context).
    @log = ();
    my $s = ctx() x 2;
    check('repeat_operator_scalar', "@log" eq "scalar");
    check('repeat_operator_result', $s eq "xx");
}

# ── Section 3: other callCtx_-setting call sites beyond plain list
#    assignment — lvalue slice RHS, hash-from-list RHS ───────────────────
{
    @log = ();
    my @arr = (0, 0, 0);
    @arr[0, 1] = (ctx() eq "x", 1);
    check('lvalue_array_slice_rhs_scalar', "@log" eq "scalar");
}
{
    @log = ();
    my %h = (k => (ctx() eq "x"));
    check('hash_from_list_rhs_scalar', "@log" eq "scalar");
}

# ── Section 4: nested operators — a call buried two levels deep under
#    scalar-forcing operators still correctly sees scalar context ───────
{
    @log = ();
    my @a = ((ctx() eq "x") && 1);
    check('nested_binop_under_and_scalar', "@log" eq "scalar");
}

# ── Section 5: regression — a call that IS the direct, top-level RHS
#    (no operator involved at all) still correctly sees list context,
#    unaffected by this fix ───────────────────────────────────────────────
{
    my @a = (listret());
    check('regression_direct_call_list_context', "@a" eq "1 2 3");
}

# ── Section 6: regression — D12's print/printf-specific fix still works:
#    a direct call argument to print sees list context, while a call
#    buried under `eq` inside a print argument still sees scalar context ──
{
    @log = ();
    print pctx(), "\n";
    check('regression_print_direct_call_list', "@log" eq "list");
}
{
    @log = ();
    print pctx() eq "z" ? "t" : "f", "\n";
    check('regression_print_binop_scalar', "@log" eq "scalar");
}

# ── Section 7: regression — ordinary arithmetic/string/comparison
#    results are still computed correctly (this fix only touches
#    context propagation, not the operators' own results) ───────────────
{
    check('regression_arithmetic_result', (3 + 4) == 7);
    check('regression_string_result', ("foo" . "bar") eq "foobar");
    check('regression_comparison_result', (5 == 5) == 1);
    check('regression_cmp_result', ("a" cmp "b") == -1);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d88_callctx_leak_done\n";
