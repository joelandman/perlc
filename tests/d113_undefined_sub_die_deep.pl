#!/usr/bin/perl
# Deep test for D113: an undefined sub call inside a non-main package
# must die with the *qualified* name (e.g. "&Other::nosuch_d113"), not
# "&main::...", matching real Perl's package-aware error message. Real
# defined subs (including a qualified cross-package call) still work
# fine right up until the undefined one is reached — regression coverage
# for the normal call path alongside the new die behavior.
#
# The `use Some::Unresolvable::Module;` half of D113 (a compile-time
# "Can't locate ... in @INC" error) is a compile failure, not a runtime
# output difference, so it isn't exercised by this harness-compared
# runtime test — see TESTS.md / MVP_ROADMAP.md for that verification.
use strict;
use warnings;

sub real_sub { my ($a, $b) = @_; return $a + $b; }
print "real_sub(2,3)=", real_sub(2, 3), "\n";

package Other;
sub other_sub { return "other"; }

package main;
print "Other::other_sub()=", Other::other_sub(), "\n";

package Other;
nosuch_d113_qualified();
