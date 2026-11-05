#!/usr/bin/env perl
# Deep pin for the unboxed `/` zero-divisor guard (perf round 2, E1).
# Exercises the guard in every unboxed division position with die
# locations, nesting, loops and inlined subs — all byte-compared against
# real perl, including the exact "... at FILE line N." location text and
# the uncaught exit-at-255 case at the very end.
use strict;
use warnings;

my @out;

my $z  = 0.0;

# 1. paren-grouped divisor, nonzero: exact arithmetic through the look-through.
{ my $r = 30 / 10.0 + 0.5 / (2); push @out, sprintf( 'r=%.4f', $r ); }

# 3. mid-loop die: the divisor goes zero on the third iteration; the first
#    two writes happened in both implementations, the third didn't.
#    (Divisors are float VARS; a ternary-initialized var and an element
#    divisor both fall back to the pre-existing boxed perl_div path, which
#    is a separate behavior, not guarded here.)
{
    my $sum = 0.0;
    eval {
        for my $k ( 0 .. 3 ) {
            my $dv = 2.0;
            $dv = 0.0 if $k == 2;
            my $q = 100 / ($dv);
            $sum += $q;
        }
    };
    push @out, sprintf( 'sum3=%.4f', $sum ), 'caught3=' . ( $@ ne '' ? 'yes' : 'no' );
}

# 4. array-ref element compound-assign loop: zero divisor mid-row.
{
    my $row = [ 10.0, 20.0, 30.0 ];
    my $d0  = 5.0;
    my $dz  = 0.0;
    eval { $row->[0] /= ($d0); $row->[1] /= ($dz); $row->[2] /= ($d0); };
    my $s = 0;
    $s += $_ for @$row;
    push @out, "rowsum=$s", 'caught4=' . ( $@ ne '' ? 'yes' : 'no' );
}

# 5. nested evals: inner catches, outer must not see $@ afterwards.
{
    my $ok = 0;
    eval {
        eval { my $t = 1 / ($z); };
        $ok = 1 unless $@;
    };
    push @out, "nested_ok=$ok", 'outer_caught=' . ( $@ ne '' ? 'yes' : 'no' );
}

# 6. division chain: the first zero divisor in the chain decides which die
#    fires (the sub-expression is emitted in order).
{
    my $d1 = 0.0;
    my $d2 = 2.0;
    eval { my $k = ( 1 + 2 ) / ($d1) + 8 / ($d2); };
    push @out, 'chain_caught=' . ( $@ ne '' ? 'yes' : 'no' );
    eval { my $k = ( 1 + 2 ) / ($d2) + 8 / ($d1); };
    push @out, 'chain2_caught=' . ( $@ ne '' ? 'yes' : 'no' );
}

# 7. literal paren zero divisor `1 / (0)`.
eval { my $t = 1 / (0); };
push @out, 'lit_caught=' . ( $@ ne '' ? 'yes' : 'no' );

# 8. exact $@ text for a value-position die (message + location).
{
    my $t;
    eval { $t = 8 / ($z); };
    push @out, 'at=' . ( $@ =~ /^Illegal division by zero at \S+ line \d+\.?$/s ? 'loc' : 'bad' );
}

print "$_\n" for @out;

# 9. the finale is UNCAUGHT: real perl prints the die to stderr and exits
#     255; this pins that the guard's die is identical when nobody catches
#     it, including the location line. Keep last.
my $final = 5 / ($z);
print "unreachable=$final\n";
