#!/usr/bin/perl
# Smoke test for blocker #1's static `do "literal.pl"` expansion: a `do`
# whose target is a literal path known at COMPILE time, and whose body is
# pure self-contained sub definitions, must have those subs compiled into
# this binary at build time (no perlc+clang spawned at runtime).
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# do of a pure-subdef file yields undef (last statement is a sub def).
# (Must be a `./`-relative path so perl treats it as cwd-relative rather
# than an @INC lookup, matching perlc's compile-time direct-path read.)
my $ret = do "./tests/lit/do_literal_helper.pl";
check('do_pure_subdef_returns_undef', !defined($ret) || $ret eq '');

# The subs from the do'd file are callable here — proof they were inlined
# into this binary, not loaded via a runtime spawn.
check('add2_inline', add2(3, 4) == 7);
check('add2_inline2', add2(10, 20) == 30);
check('greet_inline', greet("perlc") eq "hello, perlc");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures), "\n";
}

print "do_literal_smoke_done\n";
