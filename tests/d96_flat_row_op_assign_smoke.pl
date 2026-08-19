#!/usr/bin/env perl
# D96: compound-assign (+=, -=) on a 2D FLAT_ARRAY row element inside a sub
# whose array-ref parameter is DerefAV-cached.  Previously crashed (segfault
# from perl_array_get_ref(null,...)) or silently corrupted data (lazy-convert
# disconnecting the write from the flat double[] storage the read path uses).
# Smoke: one check — a sub mutates @bodies->[i][3] in a nested loop and the
# result must match real Perl.
sub advance {
    my ( $nb, $b, $dt ) = @_;
    for my $i ( 0 .. $nb - 1 ) {
        for my $j ( $i + 1 .. $nb - 1 ) {
            my $dx = $b->[$i][0] - $b->[$j][0];
            my $distance = sqrt( $dx * $dx );
            my $mag = $dt / ($distance * $distance * $distance);
            $b->[$i][3] -= $dx * $b->[$j][6] * $mag;
            $b->[$j][3] += $dx * $b->[$i][6] * $mag;
        }
    }
}
my @bodies = (
    [ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 39.0 ],
    [ 4.84, -1.16, -0.103, 0.6, 2.8, -0.025, 0.037 ],
    [ 8.34, 4.12, -0.40, -1.01, 1.82, 0.0084, 0.011 ],
);
advance( 3, \@bodies, 0.01 );
my $expected = "0.583360526617313";
my $got = sprintf("%.15g", $bodies[1][3]);
print "d96_flat_row_op_assign_smoke=", ($got eq $expected ? "ok" : "FAIL got=$got exp=$expected"), "\n";