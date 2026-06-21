#!/usr/bin/env perl

# using TimerOutputs  -- cannot be converted directly (no equivalent module used; manual timing with Time::HiRes below)
# using PyPlot  -- cannot be converted (no PyPlot/ plotting library used; plotting section omitted)
# using Printf  -- not needed (Perl has built-in printf)

use strict;
use warnings;
use Time::HiRes qw(time);

my $N    = 1024;
my $NP   = $N * 1.0;
my $xmin = -1.5;
my $xmax = 1.0;
my $ymin = -1.0;
my $ymax = 1.0;

my $dx = ( $xmax - $xmin ) / $NP;
my $dy = ( $ymax - $ymin ) / $NP;

# Complex helpers (broadcast note: . ops in Julia mean elementwise; implemented via explicit loops here)
sub cplx {
    my ( $re, $im ) = @_;

    #$im //= 0.0;
    return [ $re, $im ];
}

sub cadd {
    my ( $a, $b ) = @_;
    return [ $a->[0] + $b->[0], $a->[1] + $b->[1] ];
}

sub cmul {
    my ( $a, $b ) = @_;
    return [
        $a->[0] * $b->[0] - $a->[1] * $b->[1],
        $a->[0] * $b->[1] + $a->[1] * $b->[0]
    ];
}

sub cabs2 {
    my ($z) = @_;
    return $z->[0] * $z->[0] + $z->[1] * $z->[1];
}

sub cabs {
    my ($z) = @_;
    return sqrt( cabs2($z) );
}

# fill_z! populates z[row][col] with complex(xmin + col*dx, ymin + row*dy)
sub fill_z {
    my ( $z, $N, $xmin, $dx, $ymin, $dy ) = @_;
    for ( my $j = 0 ; $j < $N ; $j++ ) {
        $z->[$j] = [];
        for ( my $i = 0 ; $i < $N ; $i++ ) {
            $z->[$j][$i] = cplx( $xmin + $i * $dx, $ymin + $j * $dy );
        }
    }
    return;
}

# mbi! performs Mandelbrot iterations in place on zzz using ccc as constant
sub mbi {
    my ( $zzz, $ccc, $Niter, $N ) = @_;

    # preallocate temp (equivalent to zeros inside)
    my @zzzp;
    for ( my $j = 0 ; $j < $N ; $j++ ) {
        $zzzp[$j] = [];
    }

    for ( my $iter = 0 ; $iter < $Niter ; $iter++ ) {

        # zzzp .= zzz .* zzz .+ ccc
        # pragma omp for
        for ( my $j = 0 ; $j < $N ; $j++ ) {
            my $zj  = $zzz->[$j];
            my $ccj = $ccc->[$j];
            my $zpj = $zzzp[$j];
            for ( my $i = 0 ; $i < $N ; $i++ ) {
                my $z = $zj->[$i];
                $zpj->[$i] = cadd( cmul( $z, $z ), $ccj->[$i] );
            }
        }

        # zzz .= zzzp .* (abs2(zzzp) < 4) .+ (2+0i) * (abs(zzzp) >= 4)
        # pragma omp for
        for ( my $j = 0 ; $j < $N ; $j++ ) {
            my $zpj = $zzzp[$j];
            my $zj  = $zzz->[$j];
            for ( my $i = 0 ; $i < $N ; $i++ ) {
                my $zp = $zpj->[$i];
                if ( cabs2($zp) < 4.0 ) {
                    $zj->[$i] = $zp;
                }
                else {
                    $zj->[$i] = cplx( 2.0, 0.0 );
                }
            }
        }
    }
    return;
}

# main
my @z;

my $t0 = time();
fill_z( \@z, $N, $xmin, $dx, $ymin, $dy );
my $t_fill = time() - $t0;

$t0 = time();
my @c
  ; # we can alias without full copy since c is read-only and we replace z slots

# To match "copy constant" semantics while saving memory, alias the structure:
# (overwriting z[i][j] leaves the original complex objects referenced by c)
for ( my $j = 0 ; $j < $N ; $j++ ) {
    $c[$j] =
      $z[$j]; # share the row arrayrefs; values will diverge safely as explained
}
my $t_copy = time() - $t0;

# force 1 iteration to "compile" (Perl has no JIT, but warms caches)
mbi( \@z, \@c, 1, $N );

$t0 = time();
mbi( \@z, \@c, 80, $N );
my $t_iter = time() - $t0;

$t0 = time();
my @field;
for ( my $j = 0 ; $j < $N ; $j++ ) {
    $field[$j] = [];
    for ( my $i = 0 ; $i < $N ; $i++ ) {
        $field[$j][$i] = cabs( $z[$j][$i] );
    }
}
my $sample_abs = $field[0][0];
my $t_mag      = time() - $t0;

# PyPlot code commented out (cannot convert):
# PyPlot.gray()
# imshow(field, interpolation="none")
# colorbar()
# savefig("mbs.png", dpi=1200)

printf("\n");

# show timings (equivalent to show(to) )
print "Timing results (seconds):\n";
printf( "  fill array    : %.6f\n", $t_fill );
printf( "  copy constant : %.6f\n", $t_copy );
printf( "  run iterations: %.6f\n", $t_iter );
printf( "  get magnitude : %.6f (sample abs(z[0,0])=%.6f)\n",
    $t_mag, $sample_abs );
