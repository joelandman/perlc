#!/usr/bin/perl
# In-depth test suite for D58: `our $pkgvar` did not persist/update
# across *repeated* `do FILE` calls on the same file within one process.
#
# Root cause: D24's `--do-lib` compiler mode compiles each `do` call's
# target file into an independent shared library and dlopen()s it
# separately. File-scope `our`/`my` scalars used an ordinary per-
# compilation-unit LLVM GlobalVariable for their storage — correct for a
# normal program (one compile, one process), but wrong for a do-lib:
# every separate `do` call got a *fresh*, disconnected GlobalVariable
# instance in its own freshly-dlopen()ed library, so a value written in
# one `do` call was invisible to the next, instead of the single,
# persistent, process-wide slot real Perl's package variables imply
# (`our` is just a lexical alias to one shared package scalar).
#
# Fixed (scalars only — see TESTS.md's D58 entry for the still-open
# sub-redefinition case) by routing file-scope scalar declarations,
# specifically in `--do-lib` compiles, through a new process-wide
# registry (`perl_get_or_create_global_scalar()`, keyed by the qualified
# "Package::name") instead of a GlobalVariable — the same design already
# used for cross-do-lib sub calls via `perl_register_method`/
# `perl_call_named_sub`. Gated behind a new `asDoLib_` CodeGen flag, so
# normal (non-`do`-related) compilation is completely unaffected.
#
# Deliberately uses fixed temp filenames rather than PID-based
# uniqueness (`$$` doesn't interpolate correctly inside a double-quoted
# string — D59, an unrelated bug found while writing this test).
# Deliberately no `use warnings` — Section 3 references a cross-package
# variable ($PersistPkg::val) exactly once, which real Perl's warnings
# pragma flags as "used only once: possible typo"; perlc doesn't emit
# that diagnostic (D56), so the warning-pragma-off form keeps both
# sides' stderr output empty and directly comparable.
use strict;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# ── Section 1: the original bug — bare `our $counter` accumulates ─────────
# ── across repeated `do` calls on the same file ─────────────────────────────
{
    my $libfile = "/tmp/perlc_do_persist_bare.pl";
    open(my $fh, '>', $libfile) or die "write: $!";
    print $fh 'our $counter; $counter++; return $counter;' . "\n";
    close($fh);

    my $r1 = do $libfile;
    my $r2 = do $libfile;
    my $r3 = do $libfile;
    unlink($libfile);

    check('bare_our_first_call', $r1 == 1);
    check('bare_our_second_call', $r2 == 2);
    check('bare_our_third_call', $r3 == 3);
}

# ── Section 2: an explicit initializer re-runs every call (matching real ──
# ── Perl — an initializer is an ordinary assignment, not run-once magic) ───
{
    my $libfile = "/tmp/perlc_do_persist_init.pl";
    open(my $fh, '>', $libfile) or die "write: $!";
    print $fh 'our $x = 5; $x++; return $x;' . "\n";
    close($fh);

    my $r1 = do $libfile;
    my $r2 = do $libfile;
    unlink($libfile);

    check('initializer_reruns_every_call', $r1 == 6 && $r2 == 6);
}

# ── Section 3: cross-package access to a do-lib-compiled package scalar, ──
# ── verified *within* the do'd file itself (accessing a package variable ──
# ── that only a dynamically do'd file ever declares, from the separately- ─
# ── compiled *loading* program's own static code, is a further, deeper, ──
# ── out-of-scope limitation — perlc's package-variable resolution is a ────
# ── compile-time mechanism, and the loading program here has no static ────
# ── knowledge of PersistPkg::val at all; noted in TESTS.md's D58 entry) ────
{
    my $libfile = "/tmp/perlc_do_persist_crosspkg.pl";
    open(my $fh, '>', $libfile) or die "write: $!";
    print $fh 'package PersistPkg;' . "\n";
    print $fh 'our $val = 7;' . "\n";
    print $fh 'package main;' . "\n";
    print $fh 'return ($PersistPkg::val == 7) ? "ok" : "FAIL";' . "\n";
    close($fh);

    my $r = do $libfile;
    unlink($libfile);

    check('crosspkg_access_within_do_lib', $r eq "ok");
}

# ── Section 4: regression — a single `do` call (no repetition) still ──────
# ── works correctly, matching D24's original test coverage ─────────────────
{
    my $libfile = "/tmp/perlc_do_persist_single.pl";
    open(my $fh, '>', $libfile) or die "write: $!";
    print $fh 'our $single; $single = 42; return $single;' . "\n";
    close($fh);

    my $r = do $libfile;
    unlink($libfile);

    check('single_do_call_regression', $r == 42);
}

# ── Section 5: regression — a normal (non-`do`-related) program's own ─────
# ── `our` variables are completely unaffected by this fix ──────────────────
{
    our $normal_x = 5;
    $normal_x++;
    check('normal_our_regression', $normal_x == 6);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "do_file_persistence_tests_done\n";
