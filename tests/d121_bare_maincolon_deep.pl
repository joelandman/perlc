#!/usr/bin/perl
# Deep test for D121: $::name / @::arr / %::hash (bare :: package
# prefix, Perl's shorthand for main::) must lex/parse identically to
# writing out $main::name / @main::arr / %main::hash explicitly.
#
# NB: an *undeclared* (no `our`) qualified variable — `::`-shorthand or
# spelled out `main::` alike — has its own separate, pre-existing gap
# (D110: it auto-vivifies per-scope instead of being a true cross-scope
# global; found to also apply to array/hash access, not just scalars,
# while verifying this fix — see TESTS.md's widened D110 note). This
# test declares with `our` first specifically to isolate what the `::`
# shorthand itself is responsible for — reading/writing through it must
# reach the SAME storage an explicit `main::name` would, including from
# inside a subroutine (proving it's real global storage, not a
# scope-local alloca) — without also depending on D110's separate gap.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

our $counter = 10;
check('bare_assign_read', $::counter == 10);

$::counter++;
check('bare_increment', $::counter == 11);

sub read_it { return $::counter; }
check('visible_from_sub', read_it() == 11);

sub bump_it { $::counter += 5; }
bump_it();
check('mutate_from_sub', $::counter == 16);

# equivalent to the explicit main:: form
our $other;
$main::other = "hello";
check('equivalent_to_explicit_main', $::other eq "hello");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d121_bare_maincolon_done\n";
