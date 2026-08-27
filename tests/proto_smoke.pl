#!/usr/bin/perl
# Smoke: sub prototypes — $ @ () &_ block form.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub add ($$) { $_[0] + $_[1] }
check('dollar', add(2, 3) == 5);

sub ident ($) { $_[0] }
my @a = (10, 20, 30);
check('scalar_arr', ident(@a) == 3);

sub empty () { 99 }
check('empty_paren', empty() == 99);
check('empty_bare', empty == 99);

sub gather (@) { join ",", @_ }
check('slurp', gather(1, 2, 3) eq "1,2,3");

sub myg (&@) {
    my $c = shift;
    my $n = 0;
    for my $x (@_) { $n++ if $c->($x) }
    $n;
}
check('amp_block', do { my $r = myg { $_[0] > 2 } 1, 2, 3, 4; $r == 2 });

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "proto_smoke_done\n";
