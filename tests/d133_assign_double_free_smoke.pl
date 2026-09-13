#!/usr/bin/perl
# Smoke test for D133: `my $var = EXPR` inside a bare block whose RHS is a
# string→number coercion ("x" + 0) used to emit a double free of the boxed
# RHS temp (the int/float-var Assign fallback freed the temp AND returned
# it for the statement context to free again). With the variable later read
# via == the corrupted allocator segfaulted. Repro kept minimal to the
# trigger shape; expected output matches real perl byte-for-byte.
use strict;
use warnings;
no warnings 'numeric';

sub f {
    my $acc = 0;
    {
        $acc = "x" + 0;
    }
    return $acc;
}
my $v = f();
my $x = $v == 0;
print "$x\n";
print "d133_done\n";