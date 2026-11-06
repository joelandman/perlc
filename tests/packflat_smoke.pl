use strict;
use warnings;

# Stage 42 (packflat): row-valued sub-returns pack to refcounted packed
# rows. The value is observably a REF-like value: clones SHARE it (the
# c-aliased row must see mutations made through z), element reads box on
# demand, and the alias contract matches plain \@-refs exactly.

sub fill_row {
    my ($re, $im, $res, $ims, $n) = @_;
    my @r; return \@r if $n <= 0;
    $#r = $n * 2 - 1;
    for my $i (0..$n-1) {
        $r[$i*2]   = $re + $i * $res;
        $r[$i*2+1] = $im + $i * $ims;
    }
    return \@r;
}

my @z;
push @z, fill_row(0.0, 0.5 * $_, 0.25, 0.0, 8) for (0..3);

# ref-like aliasing: two holders, mutation through one visible in the other
my @c;
for my $j (0..3) { $c[$j] = $z[$j]; }
$z[0][0] = 99.5;
print "alias: $c[0][0] $z[0][0]\n";
$z[0][0] = 0.0;
print "alias2: $c[0][0]\n";

# element reads round-trip exactly
print "elems: $z[1][0] $z[1][1] $z[1][14] $z[1][15]\n";

# lengths and scalar context
print "len: ", scalar(@{$z[1]}), "\n";

# empty-row early-return path (the boxed REF escape stays byte-identical)
my $e = fill_row(0, 0, 0, 0, 0);
print "empty: ", scalar(@{$e}), " ", (defined($e) ? "def" : "nil"), "\n";

# int-halved rows: re halves are IVs, im halves are FLOATs — the packed
# round trip must print identical digits
sub fill_ints {
    my ($n) = @_;
    my @r; $#r = $n * 2 - 1;
    for my $i (0..$n-1) { $r[$i*2] = $i + 3; $r[$i*2+1] = $i * 0.5; }
    return \@r;
}
my $ir = fill_ints(4);
print "ints: $ir->[0] $ir->[1] $ir->[2] $ir->[3]\n";
print "intsum: ", ($ir->[0] + $ir->[2]), "\n";
