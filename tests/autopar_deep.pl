#!/usr/bin/env perl
# Stage 45 (autopar) deep: a bound far over the parallel fan-out threshold
# takes the pool-parallel flat body in perlc (16-way chunked) and MUST stay
# byte-identical to the sequential computation real perl performs — chunk
# boundaries land anywhere (including mid-pair) without changing any element.
use strict; use warnings;

sub flat_fill_pair {
    my ($re_base, $im_base, $re_stride, $im_stride, $n) = @_;
    my @r;
    return \@r if $n <= 0;
    $#r = $n * 2 - 1;
    for my $i (0..$n-1) {
        $r[$i*2]   = $re_base + $i * $re_stride;
        $r[$i*2+1] = $im_base + $i * $im_stride;
    }
    return \@r;
}

my $NROW = 13005;   # deliberately NOT a round multiple of any chunk size
my $z   = flat_fill_pair(-1.5, -0.35, 1.5e-4, 3.5e-5, $NROW);
my $cc  = flat_fill_pair( 0.25,  0.75, 0.0    , 0.0    , $NROW);
my $zp  = flat_fill_pair( 0.0,   0.0,  0.0    , 0.0    , $NROW);

# kernel-1: zp .= z*z + c   (bound = 13005 >= 8192 -> pool fan-out)
for ( my $i = 0 ; $i < 13005 ; $i++ ) {
    my $z_re  = $z->[ $i * 2 ];
    my $z_im  = $z->[ $i * 2 + 1 ];
    my $cc_re = $cc->[ $i * 2 ];
    my $cc_im = $cc->[ $i * 2 + 1 ];
    my $new_re = $z_re * $z_re - $z_im * $z_im + $cc_re;
    my $new_im = 2 * $z_re * $z_im + $cc_im;
    $zp->[ $i * 2 ]     = $new_re;
    $zp->[ $i * 2 + 1 ] = $new_im;
}

# kernel-2: z .= zp .* (abs2 < 4) .+ 2+0i  (uniform if => select form)
for ( my $i = 0 ; $i < 13005 ; $i++ ) {
    my $zp_re = $zp->[ $i * 2 ];
    my $zp_im = $zp->[ $i * 2 + 1 ];
    my $abs2  = $zp_re * $zp_re + $zp_im * $zp_im;
    if ( $abs2 < 4.0 ) {
        $z->[ $i * 2 ]     = $zp_re;
        $z->[ $i * 2 + 1 ] = $zp_im;
    } else {
        $z->[ $i * 2 ]     = 2.0;
        $z->[ $i * 2 + 1 ] = 0.0;
    }
}

# chunk-boundary-sensitive spot checks plus an aggregate
print "z0=[$z->[0] $z->[1]]\n";
print "z5k=[$z->[10000] $z->[10001]]\n";
print "zend=[$z->[26008] $z->[26009]]\n";
my $sum = 0.0;
$sum += $z->[$_] foreach 0, 9, 99, 999, 4999, 9999, 19999, 26001;
printf "sum=%.6f\n", $sum;

# a second fan-out on the same rows (pool reuse path)
for ( my $i = 0 ; $i < 13005 ; $i++ ) {
    my $a = $z->[ $i * 2 ];
    my $b = $z->[ $i * 2 + 1 ];
    $zp->[ $i * 2 ]     = $a * $a + $b * $b;
    $zp->[ $i * 2 + 1 ] = $a * $b;
}
print "q0=[$zp->[0] $zp->[1]]\n";
print "qmid=[$zp->[26008] $zp->[26009]]\n";
my $qsum = 0.0;
$qsum += $zp->[$_] foreach 2, 22, 2222, 22000;
printf "qsum=%.6f\n", $qsum;
print "done\n";
