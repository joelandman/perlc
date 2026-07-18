# D93: wantarray ternary dispatch smoke test
# Verifies that wantarray() ? (LIST) : SCALAR correctly returns list in list context
# (no use warnings to avoid diagnostic differences, D56)

sub listret {
    return wantarray() ? (1,2,3) : "scalar";
}

my @a = (listret());
die "D93: array count wrong" unless scalar(@a) == 3;
die "D93: array[0] wrong" unless $a[0] == 1;
die "D93: array[1] wrong" unless $a[1] == 2;
die "D93: array[2] wrong" unless $a[2] == 3;

my $b = listret();
die "D93: scalar wrong" unless $b eq "scalar";

print "ok\n";
