#!/usr/bin/perl
# Stage 37 / Stage 36 / emitIdx de-boxing: observable-value contract.
# All shapes must reproduce real Perl byte-for-byte; the unboxing is
# transparent, but the specific patterns that exercised the new paths are
# pinned here.
use strict;
use warnings;

my @fail;
sub check {
    my ($n, $ok) = @_;
    print "$n=", ($ok ? "ok" : "FAIL"), "\n";
    push @fail, $n unless $ok;
}

# 1. self-recursive int kernel: the classic fib shape (Stage 37's target).
sub fib {
    my ($n) = @_;
    if ( $n <= 1 ) { return $n; }
    return fib( $n - 1 ) + fib( $n - 2 );
}
check('fib10', fib(10) == 55);
check('fib15', fib(15) == 610);

# 2. two-int-arg kernel, called from several literal sites.
sub addmul {
    my ( $a, $b ) = @_;
    return $a * $b + $a - $b;
}
check('am1', addmul( 3, 4 ) == 11);
check('am2', addmul( 10, 2 ) == 20);
check('am3', addmul( 7, 7 ) == 49);

# 3. param mixed with a file-scope int global in the same expression.
my $bias = 5;
sub biased { my ($q) = @_; return $q + $bias; }
check('b1', biased(1) == 6);
check('b2', biased(40) == 45);

# 4. non-int args keep the packed path (no truncation divergence).
sub passthru {
    my ($v) = @_;
    return $v;
}
check('str-arg', passthru("hello") eq "hello");
check('float-arg', passthru(2.5) == 2.5);
check('iv-arg',    passthru(7) == 7);

# 5. int args through multi-statement bodies keep integer exactness
#    (NOTE: defined() on a missing @_-unpack param is DIVERGENT per D197,
#    tracked separately — not pinned here).
sub pairsum {
    my ( $x, $y ) = @_;
    return $x + $y * 3;
}
check('pairsum', pairsum( 2, 5 ) == 17);
check('pairsum2', pairsum( 1, 1 ) == 4);

# 6. int-arg with a bit-63-safe range feeding ++/-- and comparisons.
sub tally {
    my ($k) = @_;
    my $s = 0;
    for my $i ( 1 .. $k ) { $s += $i; }
    return $s;
}
check('tally5',  tally(5) == 15);
check('tally100', tally(100) == 5050);

# 7. recursion driven by an arithmetic arg expression (the emitIdx/cond
#    de-box path: $k * 2 + 1 style nested arithmetic in index positions).
sub reach {
    my ( $k, $depth ) = @_;
    return $depth if $depth >= 4;
    my @slots;
    $slots[ $k * 2 ]     = $depth;
    $slots[ $k * 2 + 1 ] = $depth + 10;
    my $s = $slots[ $k * 2 ] + $slots[ $k * 2 + 1 ];
    return $s + reach( $k * 2, $depth + 1 );
}
# reach(1,0): each level stores (depth, depth+10) and returns their sum plus
# the recursion; levels 0..3 contribute 10,12,14,16 and the last call
# returns 4 at the depth guard. Total = 56.
check('reach', reach(1, 0) == 56);

print "done\n";
exit(@fail ? 1 : 0);