# D83: ref() returns "ARRAY" for FLAT_ARRAY-tagged scalars
# ref() should return "ARRAY" for all-array-ref types including FLAT_ARRAY

my $y = [4, 5, 6];
die "FAIL: D83a" unless ref($y) eq "ARRAY";

my $z = [1.0, 2.0, 3.0];
die "FAIL: D83b" unless ref($z) eq "ARRAY";

# Mixed-type should also return ARRAY (REF_ARRAY, not FLAT_ARRAY)
my $m = [1, "two", 3];
die "FAIL: D83c" unless ref($m) eq "ARRAY";

# Double-ref (ref to an array-ref) returns "REF"
my $s = \$y;
die "FAIL: D83d" unless ref($s) eq "REF";

# Hash ref should return HASH
my $h = {};
die "FAIL: D83e" unless ref($h) eq "HASH";

# Code ref should return CODE
sub f { return 1; }
die "FAIL: D83f" unless ref(\&f) eq "CODE";

print "ok\n";
