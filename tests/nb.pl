my $pi            = 3.141592653589793;
my $solar_mass    = 4 * $pi * $pi;
my $days_per_year = 365.24;

sub offset_momentum {
    my ( $nbodies, $bodies ) = @_;
    my ( $px, $py, $pz ) = ( 0.0, 0.0, 0.0 );
    for my $i ( 0 .. $nbodies - 1 ) {
        $px += $bodies->[$i][3] * $bodies->[$i][6];
        $py += $bodies->[$i][4] * $bodies->[$i][6];
        $pz += $bodies->[$i][5] * $bodies->[$i][6];
    }
    $bodies->[0][3] = -$px / $solar_mass;
    $bodies->[0][4] = -$py / $solar_mass;
    $bodies->[0][5] = -$pz / $solar_mass;
}

sub advance {
    my ( $nbodies, $bodies, $dt ) = @_;
    for my $i ( 0 .. $nbodies - 1 ) {
        for my $j ( $i + 1 .. $nbodies - 1 ) {
            my $dx       = $bodies->[$i][0] - $bodies->[$j][0];
            my $dy       = $bodies->[$i][1] - $bodies->[$j][1];
            my $dz       = $bodies->[$i][2] - $bodies->[$j][2];
            my $distance = sqrt( $dx * $dx + $dy * $dy + $dz * $dz );
            my $mag      = $dt / ( $distance * $distance * $distance );
            $bodies->[$i][3] -= $dx * $bodies->[$j][6] * $mag;
            $bodies->[$i][4] -= $dy * $bodies->[$j][6] * $mag;
            $bodies->[$i][5] -= $dz * $bodies->[$j][6] * $mag;
            $bodies->[$j][3] += $dx * $bodies->[$i][6] * $mag;
            $bodies->[$j][4] += $dy * $bodies->[$i][6] * $mag;
            $bodies->[$j][5] += $dz * $bodies->[$i][6] * $mag;
        }
    }
    for my $i ( 0 .. $nbodies - 1 ) {
        $bodies->[$i][0] += $dt * $bodies->[$i][3];
        $bodies->[$i][1] += $dt * $bodies->[$i][4];
        $bodies->[$i][2] += $dt * $bodies->[$i][5];
    }
}

sub energy {
    my ( $nbodies, $bodies ) = @_;
    my $e = 0.0;
    for my $i ( 0 .. $nbodies - 1 ) {
        $e += 0.5 * $bodies->[$i][6] *
          ( $bodies->[$i][3]**2 + $bodies->[$i][4]**2 + $bodies->[$i][5]**2 );
        for my $j ( $i + 1 .. $nbodies - 1 ) {
            my $dx       = $bodies->[$i][0] - $bodies->[$j][0];
            my $dy       = $bodies->[$i][1] - $bodies->[$j][1];
            my $dz       = $bodies->[$i][2] - $bodies->[$j][2];
            my $distance = sqrt( $dx * $dx + $dy * $dy + $dz * $dz );
            $e -= ( $bodies->[$i][6] * $bodies->[$j][6] ) / $distance;
        }
    }
    return $e;
}

my @bodies = (
    [ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, $solar_mass ],
    [
         4.84143144246472090e+00,
        -1.16032004402742839e+00,
        -1.03622044471123109e-01,
        1.66007664274403694e-03 * $days_per_year,
        7.69901118419740425e-03 * $days_per_year,
        -6.90460016972063023e-05 * $days_per_year,
        9.54791938424326609e-04 * $solar_mass
    ],
    [
         8.34336671824457987e+00,
         4.12479856412430479e+00,
        -4.03523417114321381e-01,
        -2.76742510726862411e-03 * $days_per_year,
        4.99852801234917238e-03 * $days_per_year,
        2.30417297573763929e-05 * $days_per_year,
        2.85885980666130812e-04 * $solar_mass
    ],
    [
         1.28943695621391310e+01,
        -1.51111514016986312e+01,
        -2.23307578892655734e-01,
        2.96460137564761618e-03 * $days_per_year,
        2.37847173959480950e-03 * $days_per_year,
        -2.96589568540237556e-05 * $days_per_year,
        4.36624404335156298e-05 * $solar_mass
    ],
    [
         1.53796971148509165e+01,
        -2.59193146099879641e+01,
         1.79258772950371181e-01,
        2.68067772490389322e-03 * $days_per_year,
        1.62824170038242295e-03 * $days_per_year,
        -9.51592254519715870e-05 * $days_per_year,
        5.15138902046611451e-05 * $solar_mass
    ],
);

my $n = int( $ARGV[0] );

offset_momentum( 5, \@bodies );
printf( "
    %.9f \n "
    , energy( 5, \@bodies ) );
for ( my $i = 1 ; $i <= $n ; $i++ ) {
    advance( 5, \@bodies, 0.01 );
}
printf( "
    %.9f \n "
    , energy( 5, \@bodies ) );
