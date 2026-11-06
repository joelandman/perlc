#!/usr/bin/env perl
# Stage 45 (autopar) smoke: the certified counted-loop shapes with small
# bounds run the sequential flat body (below the parallel threshold) and
# stay byte-identical to real perl.
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

my $NROW = 640;    # below the parallel fan-out threshold
my $z   = flat_fill_pair(-1.5, -0.35, 1.5e-4, 3.5e-5, $NROW);
my $cc  = flat_fill_pair( 0.25,  0.75, 0.0    , 0.0    , $NROW);
my $zp  = flat_fill_pair( 0.0,   0.0,  0.0    , 0.0    , $NROW);

# kernel-1: zp .= z*z + c
for ( my $i = 0 ; $i < $NROW ; $i++ ) {
    my $z_re  = $z->[ $i * 2 ];
    my $z_im  = $z->[ $i * 2 + 1 ];
    my $cc_re = $cc->[ $i * 2 ];
    my $cc_im = $cc->[ $i * 2 + 1 ];
    my $new_re = $z_re * $z_re - $z_im * $z_im + $cc_re;
    my $new_im = 2 * $z_re * $z_im + $cc_im;
    $zp->[ $i * 2 ]     = $new_re;
    $zp->[ $i * 2 + 1 ] = $new_im;
}

# kernel-2: z .= zp .* (abs2 < 4) .+ 2+0i
for ( my $i = 0 ; $i < $NROW ; $i++ ) {
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

print "z0=[$z->[0] $z->[1]]\n";
print "zmid=[$z->[998] $z->[999]]\n";
my $sum = 0.0;
$sum += $z->[$_] foreach 0, 7, 31, 127, 255, 511, 1023;
printf "sum=%.6f\n", $sum;
print "done\n";
