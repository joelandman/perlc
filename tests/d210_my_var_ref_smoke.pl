# D210: `\my %hash` / `\my @arr` — ref to a freshly-declared inline lexical.
#
# Real Perl: `\my %opt` is a reference to the fresh, empty hash declared by
# the inline `my`. perlc previously printed SCALAR(0x...) for \my %opt: the
# RefScalar emit path did `perl_ref_scalar(perl_hash_size(...))` -- a scalar
# ref to the empty hash's element count -- instead of a hash ref. \my @arr
# had the same problem with an array ref. This test exercises the correct
# behaviour (ref type + write-back) without printing addresses, so perlc and
# perl output is byte-identical.

# \my %opt -> HASH ref we can write through
my $rh = \my %opt;
$rh->{a} = 1;
$rh->{b} = 2;
print "ref=", ref($rh), "\n";
print "count=", scalar(keys %$rh), "\n";
print "a=", $rh->{a}, "\n";

# \my @arr -> ARRAY ref
my @list = ();
my $ar = \my @list;
$ar->[0] = "x";
$ar->[1] = "y";
print "arrref=", ref($ar), "\n";
print "first=", $ar->[0], "\n";

# a second fresh %each time; scalar() of keys on an empty hash is 0
my $empty = \my %none;
print "emptyref=", ref($empty), "\n";
print "empties=", scalar(keys %$empty), "\n";
