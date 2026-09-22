#!/usr/bin/perl
# Interned true/false PVs: same print/defined as W1, copies are mutable.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

check('print_t', (1 == 1) eq "1");
check('print_f', (1 == 2) eq "");
check('defined_f', defined(1 == 2));
check('not_f', (not 1) eq "");
check('copy_inc', do { my $x = (1 == 1); $x++; $x == 2 });
check('copy_assign', do { my $x = (1 == 2); $x = "z"; $x eq "z" });

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "intern_bool_smoke_done\n";
