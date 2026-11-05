#!/usr/bin/env perl
# Unboxed `/` zero-divisor die (perf round 2): single-element paren groups
# are NK::ArrayLit, and since the numeric fast paths now look through them,
# `$x / ($d)` leaves the boxed perl_div path (which dies exit(255) on a
# zero divisor) and emits a raw fdiv. Each unboxed division site therefore
# carries an inline zero-divisor guard that jumps to a catchable die with
# real Perl's message and location. This smoke pair pins the basic guarded
# shapes: value position, float-var compound-assign, array-ref compound-
# assign, and 2D flat-row compound-assign — caught by eval, results
# unchanged by the die, and $@ byte-exact. Tests only shapes that reach
# the guarded unboxed paths; the boxed perl_div exit(255) behavior is a
# separate pre-existing gap, intentionally not touched here.
use strict;
use warnings;

my @out;

my $z = 0;
my $d = 4;

# 1. value position, nonzero paren-grouped divisor: plain arithmetic.
{
    my $r = 10 / ($d);
    push @out, "v1=$r";
}
my @row = ( 100, 200 );
my @grid = ( [ 40, 50 ], [ 60, 70 ] );

# 2. value position, paren-grouped zero divisor: die caught by eval, $@
#    byte-exact (message + file + line).
my $r2 = eval { my $t = 10 / ($z); $t };
push @out, ( 'v2=' . ( defined $r2 ? $r2 : 'undef' ), 'caught2=' . ( $@ ne '' ? 'yes' : 'no' ) );

# 3. float-var compound-assign, paren-grouped zero divisor (block-scoped
#    float vars take the unboxed compound-assign path; $x observed
#    numerically only so the float-alloca promotion is not blocked).
{
    my $x  = 5.5;
    my $z2 = 0.0;
    eval { $x /= ($z2); };
    my $post = $x + 0;
    push @out, "post3=$post", 'caught3=' . ( $@ ne '' ? 'yes' : 'no' );
}

# 4. array-ref element compound-assign through a paren-grouped zero
#    divisor (float elements + Stage-18 PV* fast path).
{
    my $r  = [ 8.5, 9.5 ];
    my $z3 = 0.0;
    eval { $r->[0] /= ($z3); };
    my $post = $r->[0] + 0;
    push @out, "post4=$post", 'caught4=' . ( $@ ne '' ? 'yes' : 'no' );
}

# 5. 2D flat-row compound-assign, zero divisor (the nb velocity-update shape).
my @bodies = ( [ 1.0, 2.0, 3.0, 0.5 ], [ 4.0, 5.0, 6.0, 1.5 ] );
eval { $bodies[0][3] -= 2.0 / ($z); };
push @out, 'b3=' . $bodies[0][3], 'caught5=' . ( $@ ne '' ? 'yes' : 'no' );

# 6. int zero divisor in value position (same unboxed guard).
my $iz = 0;
eval { my $t = 7 / $iz; };
push @out, 'caught6=' . ( $@ ne '' ? 'yes' : 'no' );

# 7. -0.0 divisor: real Perl's check is a numeric == 0.0, which -0.0 fails to
#    escape, so it dies too.
my $nz = -0.0;
eval { my $t = 3 / ($nz); };
push @out, 'caught7=' . ( $@ ne '' ? 'yes' : 'no' );

# 8. dying did not corrupt earlier state and execution continues.
push @out, "row0=$row[0] g11=$grid[1][1]";

print "$_\n" for @out;
