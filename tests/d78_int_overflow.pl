# D78: Signed integer overflow auto-promotes to float
# This tests the runtime.c overflow detection in perl_add/perl_sub/perl_mul

# Test 1: Positive overflow (pos + pos = negative)
my $a = 922337203685477580;
my $b = $a + 100;
# The result should be a float (not a wrapped-around negative int)
die "FAIL: D78a" if $b < 0;
die "FAIL: D78a" if $b < 9.2e17;
die "FAIL: D78a" if $b > 9.3e17;

# Test 2: Negative overflow (neg - pos = positive)
my $c = -922337203685477580;
my $d = $c - 100;
die "FAIL: D78b" if $d > 0;
die "FAIL: D78b" if $d < -9.3e17;
die "FAIL: D78b" if $d > -9.2e17;

# Test 3: No overflow (small values)
my $e = 100;
my $f = $e + 200;
die "FAIL: D78c" if $f != 300;

# Test 4: Multiplication overflow
my $g = 1000000000;
my $h = $g * $g;  # 1e18, should overflow long long
die "FAIL: D78d" if $h < 0;
die "FAIL: D78d" if $h < 1e18;
die "FAIL: D78d" if $h > 1.1e18;

# Test 5: No multiplication overflow
my $i = 100;
my $j = $i * $i;
die "FAIL: D78e" if $j != 10000;

# Test 6: Large integer literal that overflows stoll
my $k = 9223372036854775807;
die "FAIL: D78f" if $k != 9223372036854775807;

print "ok\n";
