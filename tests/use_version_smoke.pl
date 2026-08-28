#!/usr/bin/perl
# Smoke: use v5.xx / use feature 'signatures' parse and enable signatures.
use v5.36;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub add($x, $y) { $x + $y }
check('sig_add', add(2, 3) == 5);

sub greet($name = "world") { "hi $name" }
check('sig_default', greet() eq "hi world");
check('sig_given', greet("x") eq "hi x");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "use_version_smoke_done\n";
