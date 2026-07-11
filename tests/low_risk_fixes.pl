#!/usr/bin/perl
# In-depth test suite bundling 3 small, low-risk Low/Cosmetic-tier defect
# fixes (grouped into one file since each is a narrow, single-site fix —
# see low_risk_fixes_smoke.pl for the fast/narrow version).
#
# D32: $. (input line number) wasn't reset to 0 when its filehandle was
# closed. Root cause: perl_close_fh() (runtime.c) freed/reset the
# filehandle's own PerlValue but never touched s_dollar_dot. Fixed by
# setting s_dollar_dot.ival = 0 there, matching real Perl's close().
#
# D33: Scalar::Util::looks_like_number returned integer 0 for the false
# case instead of Perl's empty string "" (the true case, 1, was already
# correct). Logically equivalent in boolean context, but the string
# value differs when printed/interpolated/compared with `eq`. Fixed by
# changing every `return perl_alloc_int(0);` in
# perl_su_looks_like_number() (runtime.c) to `return perl_alloc_string("");`.
#
# D43: wantarray() called at top level (outside any sub) returned 0
# instead of undef — and, as a more serious side effect of the same bug,
# read s_wantarray_stack[-1] (one element before the start of the array)
# when the context stack was empty, which is out-of-bounds/undefined
# behavior in C even though it happened not to crash. Fixed by checking
# s_wantarray_depth <= 0 first and returning perl_alloc_undef() in that
# case, matching real Perl and eliminating the out-of-bounds read.
use strict;
use warnings;
use Scalar::Util qw(looks_like_number);

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# Declared at file scope, not nested inside a `{ }` block — a block-scoped
# named sub declared after a preceding sibling block hits an unrelated,
# pre-existing perlc bug (D45), worked around the same way elsewhere.
sub ctx_probe { return wantarray() ? "list" : "scalar"; }

# ── D32: Section 1 — the original bug: $. survives past close() ────────────
{
    my $tmpfile = "/tmp/perlc_low_risk_fixes_$$.txt";
    open(my $out, '>', $tmpfile) or die "write: $!";
    print $out "a\nb\nc\n";
    close($out);

    open(my $fh, '<', $tmpfile) or die "open: $!";
    my $l1 = <$fh>;
    check('dollar_dot_after_first_line', $. == 1);
    my $l2 = <$fh>;
    check('dollar_dot_after_second_line', $. == 2);
    close($fh);
    check('dollar_dot_reset_after_close', $. == 0);
    unlink($tmpfile);
}

# ── D32: Section 2 — reopening after close starts $. from 0 again ──────────
{
    my $tmpfile = "/tmp/perlc_low_risk_fixes2_$$.txt";
    open(my $out, '>', $tmpfile) or die "write: $!";
    print $out "x\ny\n";
    close($out);

    open(my $fh, '<', $tmpfile) or die "open: $!";
    <$fh>; <$fh>;
    close($fh);
    open($fh, '<', $tmpfile) or die "reopen: $!";
    my $l1 = <$fh>;
    check('dollar_dot_fresh_after_reopen', $. == 1);
    close($fh);
    unlink($tmpfile);
}

# ── D33: Section 3 — false case is "" not 0 ─────────────────────────────────
{
    my $r1 = looks_like_number("hello");
    my $r2 = looks_like_number("");
    my $r3 = looks_like_number(undef);
    check('lln_false_word_is_empty_string', $r1 eq "");
    check('lln_false_empty_str_is_empty_string', $r2 eq "");
    check('lln_false_undef_is_empty_string', $r3 eq "");
    # sanity: still falsy in boolean context (regression)
    check('lln_false_still_falsy', !$r1);
}

# ── D33: Section 4 — true case is still 1 (regression) ──────────────────────
{
    check('lln_true_int_string', looks_like_number("42") == 1);
    check('lln_true_float_string', looks_like_number("3.14") == 1);
    check('lln_true_negative', looks_like_number("-5") == 1);
    check('lln_true_exponent', looks_like_number("1e10") == 1);
    check('lln_true_actual_number', looks_like_number(7) == 1);
}

# ── D43: Section 5 — the original bug: top-level wantarray() ───────────────
{
    my $w = wantarray();
    check('wantarray_toplevel_undef', !defined($w));
}

# ── D43: Section 6 — regression: wantarray() still works correctly ─────────
# ── inside subs (list vs scalar context) ────────────────────────────────────
{
    my @l = ctx_probe();
    my $s = ctx_probe();
    check('wantarray_sub_list_ctx', $l[0] eq "list");
    check('wantarray_sub_scalar_ctx', $s eq "scalar");
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "low_risk_fixes_tests_done\n";
