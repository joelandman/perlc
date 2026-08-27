#!/usr/bin/perl
# Deep: typeglob code alias with prototype, second alias, *FOO value.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

sub mul ($$) { $_[0] * $_[1] }
*prod = \&mul;
check('alias_proto', prod(3, 4) == 12);

*prod2 = \&mul;
check('alias_chain', prod2(5, 6) == 30);

check('star_pkg', "*main::mul" eq "*main::mul");
my $s = *mul;
check('star_assign_val', $s eq "*main::mul");

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "glob_deep_done\n";
