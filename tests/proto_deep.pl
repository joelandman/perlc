#!/usr/bin/perl
# Deep: prototype arity, optional ;, _, &name bypass, list flatten vs scalar.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub pair ($$) { "$_[0]:$_[1]" }
check('pair', pair(1, 2) eq "1:2");

sub opt ($;$) { defined $_[1] ? "$_[0]+$_[1]" : "$_[0]" }
check('opt_one', opt(5) eq "5");
check('opt_two', opt(5, 6) eq "5+6");

sub under (_) { $_[0] }
$_ = 42;
check('under_default', under() == 42);
check('under_arg', under(7) == 7);

check('too_few', do { eval 'sub few_a ($$) {} few_a(1)'; $@ ne "" });
check('too_many', do { eval 'sub few_b ($$) {} few_b(1,2,3)'; $@ ne "" });

sub slurpy (@) { scalar @_ }
my @b = (1, 2);
my @c = (3);
check('slurp_flat', slurpy(@b, @c) == 3);

sub takes1 ($) { $_[0] }
check('paren_arr_scalar', takes1(@b) == 2);

sub blk (&) { $_[0]->() }
check('amp_only', do { my $r = blk { 11 }; $r == 11 });

sub real_add { $_[0] + $_[1] }
check('amp_bypass', &real_add(10, 20) == 30);

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "proto_deep_done\n";
