#!/usr/bin/perl
# In-depth test suite for D64: a captured scalar that compiles to
# perlc's unboxed int/float fast path got a boxed *snapshot* of its
# value at capture time instead of sharing the real stable-pointer
# storage.
#
# Root cause: found while writing D62's own fix tests (D62 fixed
# closures capturing a *PV-boxed* `my` variable's value by clone instead
# of by reference; this defect is a separate, deeper, codegen-level gap
# D62's runtime-level fix cannot touch). A block-scoped `my $x = 0;` (or
# any int/float literal RHS) is eligible for perlc's unboxed fast-path
# optimization (`intScopes_`/`floatScopes_`, codegen.cpp — a raw i64/
# double alloca with no `PerlValue*` at all, used to skip PV-boxing
# overhead in hot arithmetic). When a closure's (or sort{}'s comparator)
# capture-collection logic encounters a name not found via the normal
# `lookupVar()`, it falls back to `lookupIntVar`/`lookupFloatVar` and
# boxes the *current* value into a fresh, one-off `PerlValue*`
# (`boxI64`/`boxF64`) purely to have something capturable — structurally
# incapable of aliasing back to the real unboxed storage, since that
# storage was never a `PerlValue*` to begin with. A **file-scope**
# `my $x = 0;` was never affected: file-scope int/float variables always
# route through `fileScalarGlobals_` (a real, PV-backed `GlobalVariable`),
# never the unboxed fast path.
#
# Fixed by having the fast-path *declaration* itself refuse the
# optimization for any name that will be captured somewhere in the
# current function: a new `collectClosureCapturedNames()` (codegen.cpp)
# walks a function body once (named sub, `AnonSub`, or the top-level
# program) collecting every scalar name referenced inside any nested
# `AnonSub`/`sort{}`-custom-comparator body, and `case NK::My:`'s
# unboxed-fast-path condition now also requires the declared name not be
# in that set — falling through to the normal, real-`PerlValue*`-backed
# declaration path instead, exactly like a string-initialized variable
# already does. The scan is deliberately over-approximate (a name merely
# reused, unrelated, elsewhere in the same function still loses the fast
# path) rather than risk misclassifying a genuinely captured variable as
# safe — a correctness bug, not just a missed optimization. Computed
# once per function at compile time; zero runtime cost, and variables
# never referenced inside any closure are completely unaffected
# (confirmed via a direct N-body-benchmark timing comparison showing no
# measurable difference).
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — AnonSub captures a block-scoped ─────────
# ── int-fast-path variable ───────────────────────────────────────────────────
{
    my $x = 0;
    my $f = sub { $x++; };
    $f->();
    $f->();
    check('anonsub_int_capture_visible', $x == 2);
}

# ── Section 2: the float equivalent ────────────────────────────────────────
{
    my $y = 0.0;
    my $f = sub { $y += 1.5; };
    $f->();
    $f->();
    check('anonsub_float_capture_visible', $y == 3.0);
}

# ── Section 3: sort{} comparator captures a block-scoped int variable ─────
{
    my $calls = 0;
    my @data = (5, 3, 8, 1, 4);
    my @sorted = sort { $calls++; $a <=> $b } @data;
    check('sort_int_capture_order', join(",", @sorted) eq "1,3,4,5,8");
    check('sort_int_capture_visible', $calls > 0);
}

# ── Section 4: regression — a non-captured int-fast-path variable in a ────
# ── tight loop is completely unaffected (the optimization itself still ────
# ── applies whenever nothing captures the name) ────────────────────────────
{
    my $sum = 0;
    for my $i (1..1000) {
        $sum += $i;
    }
    check('uncaptured_fast_path_regression', $sum == 500500);
}

# ── Section 5: the same variable name captured in one function but NOT ────
# ── in an unrelated function — each function's own capture analysis is ────
# ── independent, so the uncaptured one still gets the fast path ───────────
{
    check('same_name_captured_in_one_fn', captured_version() == 2);
    check('same_name_uncaptured_in_other_fn', uncaptured_version() == 5);
}
sub captured_version {
    my $n = 0;
    my $f = sub { $n++; };
    $f->();
    $f->();
    return $n;
}
sub uncaptured_version {
    my $n = 0;
    for (1..5) { $n++; }
    return $n;
}

# ── Section 6: two-way visibility on the fast path — an outside mutation ──
# ── after closure creation is seen inside the closure too ─────────────────
{
    my $z = 5;
    my $f = sub { return $z; };
    $z = 9;
    check('unboxed_two_way_visibility', $f->() == 9);
}

# ── Section 7: a variable captured by a closure nested two levels deep ────
{
    my $outer = 0;
    my $make_incrementer = sub {
        return sub { $outer++; };
    };
    my $inc = $make_incrementer->();
    $inc->();
    $inc->();
    check('nested_closure_int_capture', $outer == 2);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "closure_unboxed_capture_tests_done\n";
