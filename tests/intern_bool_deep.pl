#!/usr/bin/perl
# Deep: interned bool identity, read-only temp, copies stay independent.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

# W1 stringification / defined / numeric
check('eq_t', (1 == 1) eq "1");
check('eq_f', (2 < 1) eq "");
check('num_t', ((1 < 2) + 0) == 1);
check('num_f', ((2 < 1) + 0) == 0);
check('def_t', defined(1 == 1));
check('def_f', defined(1 == 2));
check('bool_if', (1 == 1) && !(1 == 2));

# Two refs to the same interned true (perl's PL_sv_yes)
check('ref_yes', \(1 == 1) == \(1 == 1));
check('ref_no',  \(1 == 2) == \(1 == 2));
check('ref_diff', \(1 == 1) != \(1 == 2));

# Assigned copies are distinct cells and mutable
{
    my $a = (1 == 1);
    my $b = (1 == 1);
    check('copy_ne_ref', \$a != \$b);
    $a++;
    check('copy_inc', $a == 2 && $b == 1);
}

# Mutating through a ref to the interned temp croaks (perl does too)
{
    my $r = \(1 == 1);
    eval { $$r++ };
    check('ro_inc', $@ && $@ =~ /Modification of a read-only value attempted/);
}
{
    my $r = \(1 == 2);
    eval { $$r = "x" };
    check('ro_assign', $@ && $@ =~ /Modification of a read-only value attempted/);
}

# Regex boolean still W1
check('re_t', ("abc" =~ /b/) eq "1");
check('re_f', ("abc" =~ /z/) eq "");

# Array of bools: clones, so ++ is allowed and doesn't poison later compares
{
    my @a = (1 == 1, 1 == 2);
    $a[0]++;
    check('arr_inc', $a[0] == 2 && $a[1] eq "");
    check('later_cmp', (1 == 1) eq "1");
}

if (@fail) { print "UNEXPECTED_FAILURES=", join(",", @fail), "\n"; die "fail\n"; }
print "intern_bool_deep_done\n";
