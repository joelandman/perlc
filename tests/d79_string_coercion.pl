# D79: String-to-number coercion does NOT auto-detect hex/octal/binary prefixes
# Only explicit hex()/oct() should do base auto-detection.
# Implicit coercion is purely decimal: only digits 0-9.

# Test 1: "0x..." prefix → treated as decimal "0" (stops at 'x')
my $a = "0x10" + 1;
die "D79:1" unless $a == 1;

# Test 2: "0b..." prefix → treated as decimal "0" (stops at 'b')
my $b = "0b1010" + 1;
die "D79:2" unless $b == 1;

# Test 3: "077" → treated as decimal 77 (NOT octal)
my $c = "077" + 1;
die "D79:3" unless $c == 78;

# Test 4: "0x" alone → decimal 0
my $d = "0x" + 1;
die "D79:4" unless $d == 1;

# Test 5: "0" alone → decimal 0
my $e = "0" + 1;
die "D79:5" unless $e == 1;

# Test 6: Normal decimal strings still work
my $f = "42" + 1;
die "D79:6" unless $f == 43;

# Test 7: Decimal with leading zeros
my $g = "0042" + 1;
die "D79:7" unless $g == 43;

# Test 8: Hex string with multiplication
my $h = "0x10" * 2;
die "D79:8" unless $h == 0;

# Test 9: String with trailing non-digit stops at first non-digit
my $i = "123abc" + 1;
die "D79:9" unless $i == 124;

# Test 10: hex() still works for explicit hex conversion
my $j = hex("10") + 1;
die "D79:10" unless $j == 17;

# Test 11: oct() still works for explicit octal conversion
my $k = oct("77") + 1;
die "D79:11" unless $k == 64;

# Test 12: Negative number in string
my $l = "-5" + 2;
die "D79:12" unless $l == -3;

# Test 13: Float string coercion (no hex auto-detect)
my $m = "0x1.5" + 1;
die "D79:13" unless $m == 1;

print "ok\n";
