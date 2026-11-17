#!/usr/bin/perl
# Deeper test for blocker #1's static `do "literal.pl"` expansion: the `do`
# target is a literal `./`-relative path known at COMPILE time whose body is
# pure self-contained sub definitions. Those subs must be compiled into this
# binary at build time (no perlc+clang spawned at runtime), and must be able
# to reference each other and their own local lexicals.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

# `do` of a pure-subdef file; the last statement is a sub def so it yields undef.
my $ret = do "./tests/lit/do_literal_helper.pl";
check('do_ret_undef', !defined($ret) || $ret eq '');

# Basic inlined subs.
check('add2_1', add2(3, 4) == 7);
check('add2_2', add2(10, 20) == 30);
check('greet_1', greet("perlc") eq "hello, perlc");

# Subs from the do'd file are mutually callable — proof they share one
# inlined module, not separate runtime loads.
check('greet_add', greet("add:" . add2(1, 2) . "!") eq "hello, add:3!");

# do'd subs used with edge-case arguments and in concatenation context.
check('add2_neg', add2(-5, 2) == -3);
check('add2_big', add2(1000000, 2000000) == 3000000);
check('greet_multi', greet("a") . greet("b") eq "hello, ahello, b");

if (@failures) {
    die "UNEXPECTED FAILURES: " . join(",", @failures), "\n";
}

print "do_literal_deep_done\n";
