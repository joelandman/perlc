#!/usr/bin/perl
# In-depth test suite for Carp::croak catchability.
#
# Root cause: perl_carp_croak() (runtime.c) called fprintf(stderr,...) then
# exit(1) directly, completely bypassing the die/eval/longjmp mechanism.
# The standard Carp usage pattern — eval { croak(...) } to turn a library
# error into a catchable exception — killed the entire process under
# perlc instead of setting $@ and continuing.
#
# Fixed by routing perl_carp_croak() through the existing perl_die()
# function instead of its own fprintf+exit logic. perl_die() already had
# the correct dual behavior: longjmp to the nearest eval (setting $@) if
# one is active, or print to stderr and exit(1) at top level if not — croak
# just needed to reuse it instead of duplicating (and getting wrong) the
# same logic. codegen.cpp already routes both `croak` and `confess` through
# perl_carp_croak, so both are fixed by this one change.
#
# All checks below use index($@, "substring") rather than printing/
# comparing the raw $@ value directly: real Perl's $@ includes an
# "at FILE line N.\n\t...called at..." suffix that perlc's does not add
# (a separate, pre-existing, unrelated simplification — Carp's location-
# reporting is not implemented, only its catchability is fixed here), so
# printing $@ verbatim would make this test diverge from real Perl in the
# harness's byte-for-byte comparison for a reason unrelated to what this
# fix actually covers.
#
# NOT exercised here (inherently untestable from within a self-checking
# .pl file — a real process exit would abort the rest of the checks, and
# was instead verified manually): croak()/confess() with NO enclosing eval
# still correctly terminates the process, matching plain die()'s existing
# behavior — confirmed with:
#   perl:  `perl -MCarp -e 'sub bad{croak "x"} print "before\n"; bad();'`
#          prints "before", dies, exit 255
#   perlc: same script compiled and run — prints "before", dies, exit 1
#   (both correctly stop before any further output; exit-code convention
#   differs between real Perl (255) and perlc (1), matching plain die()'s
#   pre-existing, already-different exit convention — not a regression
#   introduced by this fix)
#
# Also NOT exercised here: `carp()` (the warn-only Carp variant, as opposed
# to croak/confess). It's unaffected by this fix (only perl_carp_croak was
# touched, not perl_carp_carp) and was manually confirmed to still just
# warn-and-continue (doesn't die, doesn't set $@). It's excluded from this
# byte-for-byte-compared suite because any carp() call writes a warning to
# stderr, and real Perl's warning text always includes a "at FILE line N."
# location suffix that perlc's does not add (the same pre-existing,
# unrelated missing-location-info gap noted above for $@) — so any test
# exercising carp()'s actual output can never byte-match real Perl here,
# regardless of message content. Attempted to route around it by
# redirecting STDERR mid-script (`open(STDERR, ...)`), but perlc's `open()`
# doesn't support reopening/duping a bareword filehandle as the target
# (parse error) — a separate, narrower parser gap not worth chasing for
# this test suite; logged as part of D52's investigation instead.
use strict;
use warnings;
use Carp;

my @failures;

sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

sub simple_croak { croak("boom"); }
sub level3 { croak("deep error"); }
sub level2 { level3(); }
sub level1 { level2(); }
sub bad_confess { confess("confess error"); }

# ── Section 1: the original bug — croak caught by an enclosing eval ────────
{
    my $caught = 0;
    my $msg = "";
    eval { simple_croak(); };
    if ($@) { $caught = 1; $msg = $@; }
    check('croak_caught_by_eval', $caught);
    check('croak_message_content', index($msg, "boom") >= 0);
}

# ── Section 2: croak several call-frames deep, still caught at the top ─────
{
    my $msg = "";
    eval { level1(); };
    $msg = $@ if $@;
    check('croak_deep_call_chain_caught', index($msg, "deep error") >= 0);
}

# ── Section 3: confess routes through the same fix (shares perl_carp_croak) ─
{
    my $msg = "";
    eval { bad_confess(); };
    $msg = $@ if $@;
    check('confess_caught_by_eval', index($msg, "confess error") >= 0);
}

# ── Section 4: nested eval — inner croak caught by the inner eval only ─────
{
    my $inner_msg = "";
    my $outer_msg = "";
    eval {
        eval { croak("inner"); };
        $inner_msg = $@ if $@;
        croak("outer") if $@;
    };
    $outer_msg = $@ if $@;
    check('nested_eval_inner_catch', index($inner_msg, "inner") >= 0);
    check('nested_eval_outer_catch', index($outer_msg, "outer") >= 0);
}

# ── Section 5: croak with no message defaults to something non-empty ───────
{
    my $msg = "";
    eval { croak(); };
    $msg = $@ if $@;
    check('croak_no_args_still_catchable', length($msg) > 0);
}

# ── Section 6: execution fully continues after a caught croak — state from ─
# ── before the eval is preserved, and code after it runs normally ──────────
{
    my $counter = 0;
    for my $i (1..3) {
        eval { croak("loop iteration $i") if $i == 2; };
        $counter++;
    }
    check('execution_continues_across_loop', $counter == 3);
}

# ── Section 7: a caught croak's error can be re-thrown via die ─────────────
{
    my $final_msg = "";
    eval {
        eval { croak("original"); };
        die $@ if $@;
    };
    $final_msg = $@ if $@;
    check('rethrow_after_croak', index($final_msg, "original") >= 0);
}

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}

print "carp_croak_eval_tests_done\n";
