#!/usr/bin/perl
# In-depth test suite for D74: `substr($str, $off, $len, $replacement)`
# (the 4-arg in-place replacement form) was a silent no-op.
#
# Root cause: the parser's `substr` handling (parser.cpp) already accepted
# up to 4 comma-separated arguments (`while (args.size() < 4 ...)`), but
# `CodeGen::emitExpr`'s `NK::SubstrFunc` case (codegen.cpp) only ever
# checked for `args.size() >= 3` — there was no branch at all for a 4th
# argument, so `substr($str,$off,$len,$repl)` compiled and ran identically
# to the 3-arg read-only form: it computed and returned the extracted
# substring, but never touched $str, and the 4th argument was evaluated
# for nothing (or not evaluated at all) and silently discarded.
#
# Fixed by adding an `args.size() >= 4` branch: it computes the OLD
# substring value first (via the same `perl_substr3` runtime call the
# 3-arg form already uses — this is real Perl's documented 4-arg substr
# return value), then performs the actual in-place mutation via
# `perl_substr_replace` — the same runtime function the pre-existing 3-arg
# lvalue-assignment form (`substr($str,$off,$len) = $val`) already used,
# reusing its "stable pointer" reliance (the PerlValue* returned by
# evaluating a plain scalar-variable expression IS the variable's own
# storage, not a clone, so mutating it in place correctly reaches back
# into the original variable).
#
# NOTE: negative-length substr (`substr($s, $off, -$n, ...)`) is
# deliberately NOT exercised here — found while writing this test that it
# reproduces a separate, pre-existing, already-logged defect (TESTS.md
# D77: substr negative-length/far-negative-start mishandled) identically
# in the plain 3-arg *read-only* substr form too, confirming it's
# unrelated to this fix (this fix reuses the exact same bounds-calculation
# logic the read-only form already had, inheriting the same pre-existing
# bug rather than introducing a new one). Not fixed here.
use strict;
use warnings;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original repro ───────────────────────────────────────────
{
    my $u = "Hello";
    substr($u, 1, 3, "XYZ");
    check('basic_4arg_replace', $u eq "HXYZo");
}

# ── Section 2: the 4-arg form's return value is the OLD substring ─────────
{
    my $s = "Hello, World!";
    my $old = substr($s, 7, 5, "Perl!");
    check('return_value_is_old_substring', $old eq "World");
    check('string_mutated_in_place', $s eq "Hello, Perl!!");
}

# ── Section 3: replacement shorter than the replaced region ────────────────
{
    my $s = "abcdefgh";
    substr($s, 2, 4, "X");
    check('replacement_shorter', $s eq "abXgh");
}

# ── Section 4: replacement longer than the replaced region ─────────────────
{
    my $s = "abcdefgh";
    substr($s, 2, 2, "XYZ123");
    check('replacement_longer', $s eq "abXYZ123efgh");
}

# ── Section 5: negative offset (counts from the end of the string) ────────
{
    my $s = "Hello, World!";
    substr($s, -6, 5, "Perl!");
    check('negative_offset', $s eq "Hello, Perl!!");
}

# ── Section 6: empty replacement (deletion) ─────────────────────────────────
{
    my $s = "Hello, World!";
    substr($s, 5, 7, "");
    check('empty_replacement_deletes', $s eq "Hello!");
}

# ── Section 7: replacement to the end of the string (len covers the tail) ──
{
    my $s = "Hello, World!";
    substr($s, 7, 6, "Perl");
    check('replace_to_end', $s eq "Hello, Perl");
}

# ── Section 8: regression — 3-arg lvalue-assignment form (substr(...)=val)
#    still works, a separate, pre-existing code path from this fix's ──────
{
    my $s = "Hello, World!";
    substr($s, 7, 5) = "Perl!";
    check('lvalue_assignment_regression', $s eq "Hello, Perl!!");
}

# ── Section 9: regression — 3-arg and 2-arg read-only forms are unaffected
#    and don't mutate the original string ──────────────────────────────────
{
    my $s = "Hello, World!";
    my $sub = substr($s, 7, 5);
    check('readonly_3arg_regression_unchanged', $s eq "Hello, World!");
    check('readonly_3arg_regression_value', $sub eq "World");
}
{
    my $s = "Hello, World!";
    my $sub = substr($s, 7);
    check('readonly_2arg_regression_unchanged', $s eq "Hello, World!");
    check('readonly_2arg_regression_value', $sub eq "World!");
}

# ── Section 10: multiple sequential 4-arg replacements on the same var ────
{
    my $s = "aaaa";
    substr($s, 0, 1, "b");
    substr($s, 1, 1, "c");
    check('sequential_replacements', $s eq "bcaa");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "d74_substr_4arg_done\n";
