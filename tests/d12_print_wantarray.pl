#!/usr/bin/perl
# In-depth test suite for D12: `wantarray` context was not propagated into
# `print`/`printf` call arguments.
#
# Root cause: `print`/`say`/`printf`'s codegen (codegen.cpp) evaluated each
# argument via a plain `emitExpr()` call with no regard for `callCtx_` — the
# compiler's existing flag for "the expression about to be evaluated is in
# list context, so a sub call within it should see wantarray()==true".
# Every other list-context call site (list assignment, lvalue slices, etc.)
# already set `callCtx_ = 1` around its RHS; print/printf's argument
# evaluation just never did, so a sub called as a print/printf argument
# always saw scalar context regardless of print's own (always-list, per
# real Perl) context for its arguments.
#
# Fixed by setting `callCtx_ = 1` around each print/printf argument's
# evaluation (in all of: print-to-filehandle, print-to-stdout single-arg,
# print-to-stdout multi-arg, and printf's format+args). One subtlety found
# and fixed while testing this: `callCtx_` is a *one-shot* flag — the
# `Call` node's own codegen reads it once and immediately resets it to 0,
# so it must be set fresh before *each* argument in a multi-argument
# print/printf, not just once before the whole argument list (setting it
# once before a loop of N arguments only actually reached the *first* call
# in the list — confirmed directly: `print "a=",ctx()," b=",ctx(),"\n"`
# initially still showed the second `ctx()` call seeing scalar context).
#
# Also found while writing this test, NOT fixed here (separate, unrelated
# defect, logged as TESTS.md D86): `print {$fh} LIST` (the brace-block
# filehandle form) is misparsed as a hash-literal argument. All filehandle
# checks below use the working plain `print $fh LIST` form instead.
use strict;
use warnings;
use feature 'say';

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

my @seen;
sub ctx {
    push @seen, (!defined(wantarray) ? "void" : (wantarray ? "list" : "scalar"));
    return "x";
}

# ── Section 1: the original repro — single-arg print/printf ────────────────
{
    @seen = ();
    print ctx(), "\n";
    check('print_single_arg', $seen[-1] eq "list");
}
{
    @seen = ();
    printf("%s\n", ctx());
    check('printf_single_arg', $seen[-1] eq "list");
}

# ── Section 2: multiple sub calls in one print/printf's argument list —
#    the one-shot-flag bug found while writing this test ──────────────────
{
    @seen = ();
    print "a=", ctx(), " b=", ctx(), "\n";
    check('print_multi_arg_first_call', $seen[0] eq "list");
    check('print_multi_arg_second_call', $seen[1] eq "list");
}
{
    @seen = ();
    printf("%s and %s\n", ctx(), ctx());
    check('printf_multi_arg_first_call', $seen[0] eq "list");
    check('printf_multi_arg_second_call', $seen[1] eq "list");
}

# ── Section 3: say ─────────────────────────────────────────────────────────
{
    @seen = ();
    say ctx();
    check('say_single_arg', $seen[-1] eq "list");
}

# ── Section 4: print to an explicit filehandle (STDOUT and a scalar-var
#    filehandle, plain non-brace form) ──────────────────────────────────────
{
    @seen = ();
    print STDOUT ctx(), "\n";
    check('print_stdout_explicit', $seen[-1] eq "list");
}
{
    @seen = ();
    open(my $fh, '>', '/tmp/d12_test_fh_out.txt') or die;
    print $fh ctx(), "\n";
    close($fh);
    unlink('/tmp/d12_test_fh_out.txt');
    check('print_scalar_filehandle', $seen[-1] eq "list");
}

# ── Section 5: nested — a sub that itself returns the result of calling
#    ctx() must still see list context propagate through to ctx() ────────
{
    @seen = ();
    sub outer_ctx { return ctx(); }
    print outer_ctx(), "\n";
    check('print_nested_call', $seen[-1] eq "list");
}

# ── Section 6: regressions — genuine scalar/list context elsewhere must
#    remain correct and unaffected by this fix ─────────────────────────────
# NOTE: a bare-statement "void context" regression check (`ctx();` with no
# assignment/print) is deliberately NOT included here — found while writing
# this test that perlc's wantarray model has no void state at all (it's
# binary scalar/list throughout), so a bare call incorrectly reports
# scalar context instead of `undef`. That's a separate, pre-existing, more
# architectural gap, logged as TESTS.md D87, NOT part of D12's scope (which
# is specifically about scalar-vs-list propagation into print/printf args).
{
    @seen = ();
    my $s = ctx();
    check('scalar_assignment_regression', $seen[-1] eq "scalar");
}
{
    @seen = ();
    my @a = ctx();
    check('list_assignment_regression', $seen[-1] eq "list");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d12_print_wantarray_done\n";
