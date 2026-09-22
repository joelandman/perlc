# Deep: nested for + inlined cplx, N large enough to have overflowed
# an 8MB stack at -O0 before entry-block alloca hoisting (N=512).
sub cplx { my ($re, $im) = @_; return [$re, $im]; }
my $N = 64;
my @z;
for (my $j = 0; $j < $N; $j++) {
    $z[$j] = [];
    for (my $i = 0; $i < $N; $i++) {
        $z[$j][$i] = cplx($i * 0.25, $j * 0.25);
    }
}
print "n=$N z00=$z[0][0][0] z63=$z[63][63][0] z12=$z[1][2][0]\n";
my $s = 0;
for (my $j = 0; $j < $N; $j++) {
    for (my $i = 0; $i < $N; $i++) {
        $s += $z[$j][$i][0] + $z[$j][$i][1];
    }
}
print "sum=$s\n";
print "done\n";
