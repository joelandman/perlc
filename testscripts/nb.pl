#!/usr/bin/perl
use strict;
use warnings;

use constant pi            => 3.141592653589793;
use constant solar_mass    => 4 * pi * pi;
use constant days_per_year => 365.24;
my $n       = int( $ARGV[0] // 0 );
my $NBODIES = 5;

my @x = (
    0, 4.84143144246472090e+00, 8.34336671824457987e+00,
    1.28943695621391310e+01, 1.53796971148509165e+01
);
my @y = (
    0, -1.16032004402742839e+00, 4.12479856412430479e+00,
    -1.51111514016986312e+01, -2.59193146099879641e+01
);
my @z = (
    0, -1.03622044471123109e-01, -4.03523417114321381e-01,
    -2.23307578892655734e-01, 1.79258772950371181e-01
);
my @vx = (
    0,
    1.66007664274403694e-03 * days_per_year,
    -2.76742510726862411e-03 * days_per_year,
    2.96460137564761618e-03 * days_per_year,
    2.68067772490389322e-03 * days_per_year
);
my @vy = (
    0,
    7.69901118419740425e-03 * days_per_year,
    4.99852801234917238e-03 * days_per_year,
    2.37847173959480950e-03 * days_per_year,
    1.62824170038242295e-03 * days_per_year
);
my @vz = (
    0,
    -6.90460016972063023e-05 * days_per_year,
    2.30417297573763929e-05 * days_per_year,
    -2.96589568540237556e-05 * days_per_year,
    -9.51592254519715870e-05 * days_per_year
);
my @mass = (
    solar_mass,
    9.54791938424326609e-04 * solar_mass,
    2.85885980666130812e-04 * solar_mass,
    4.36624404335156298e-05 * solar_mass,
    5.15138902046611451e-05 * solar_mass
);

sub offset_momentum {
    my ( $px, $py, $pz ) = ( 0.0, 0.0, 0.0 );
    for my $i ( 0 .. $NBODIES - 1 ) {
        $px += $vx[$i] * $mass[$i];
        $py += $vy[$i] * $mass[$i];
        $pz += $vz[$i] * $mass[$i];
    }

    $vx[0] = -$px / solar_mass;
    $vy[0] = -$py / solar_mass;
    $vz[0] = -$pz / solar_mass;
}

sub advance {
    my ($dt) = @_;

    #pragma omp parallel for
    for my $i ( 0 .. $NBODIES - 1 ) {
        for my $j ( $i + 1 .. $NBODIES - 1 ) {
            my $dx   = $x[$i] - $x[$j];
            my $dy   = $y[$i] - $y[$j];
            my $dz   = $z[$i] - $z[$j];
            my $dist = sqrt( $dx * $dx + $dy * $dy + $dz * $dz );
            my $mag  = $dt / ( $dist * $dist * $dist );
            $vx[$i] -= $dx * $mass[$j] * $mag;
            $vy[$i] -= $dy * $mass[$j] * $mag;
            $vz[$i] -= $dz * $mass[$j] * $mag;
            $vx[$j] += $dx * $mass[$i] * $mag;
            $vy[$j] += $dy * $mass[$i] * $mag;
            $vz[$j] += $dz * $mass[$i] * $mag;
        }
    }
    for my $i ( 0 .. $NBODIES - 1 ) {
        $x[$i] += $dt * $vx[$i];
        $y[$i] += $dt * $vy[$i];
        $z[$i] += $dt * $vz[$i];
    }
}

sub energy {
    my $e = 0.0;
    for my $i ( 0 .. $NBODIES - 1 ) {
        $e += 0.5 * $mass[$i] * ( $vx[$i]**2 + $vy[$i]**2 + $vz[$i]**2 );
        for my $j ( $i + 1 .. $NBODIES - 1 ) {
            my $dx   = $x[$i] - $x[$j];
            my $dy   = $y[$i] - $y[$j];
            my $dz   = $z[$i] - $z[$j];
            my $dist = sqrt( $dx * $dx + $dy * $dy + $dz * $dz );
            $e -= $mass[$i] * $mass[$j] / $dist;
        }
    }
    $e;
}

offset_momentum();
printf "
%.9f \n "
  , energy();
for ( 1 .. $n ) {
    advance(0.01);
}
printf "
%.9f \n "
  , energy();
