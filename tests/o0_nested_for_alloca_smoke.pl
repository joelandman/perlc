# Nested C-style for + inlined 2-arg sub (cplx). At -O0, param/my
# allocas used to be emitted inside the inner loop and overflowed
# the stack around N=512 (mbs fill_z SIGSEGV in perl_mul).
sub cplx { my ($re, $im) = @_; return [$re, $im]; }
my $N = 8;
my @z;
for (my $j = 0; $j < $N; $j++) {
    $z[$j] = [];
    for (my $i = 0; $i < $N; $i++) {
        $z[$j][$i] = cplx($i * 0.5, $j * 0.5);
    }
}
print "z12=$z[1][2][0]\n";
print "done\n";
