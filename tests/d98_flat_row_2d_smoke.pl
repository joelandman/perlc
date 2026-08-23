# D98 smoke: adjacent-form compound-assign on a FLAT_ARRAY row must not crash.
my @P = ([1.0,2.0,3.0],[4.0,5.0,6.0]);
for my $i (0..1) { for my $k (0..2) { $P[$i][$k] += 0.5; } }
printf("ok=%.3f\n", $P[1][1]);
