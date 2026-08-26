# W1 I64 fast path (deep): small-IV bitwise on int vars, constant folds,
# constant-count logical shifts, variable shifts (boxed fallback),
# declaration propagation.
# NOTE (pre-existing gap, out of W1 scope): results with bit 63 set print as
# UNSIGNED in real Perl (~12 → 18446744073709551603, 1 << 63 →
# 9223372036854775808) but the runtime has no UV storage (prints IV negative).
# Those forms are deliberately absent from this test.
my $a = 123456789;
my $b = 987654321;
print($a & $b, "\n");
print($a | $b, "\n");
print($a ^ $b, "\n");
my $c = ($a & $b) + ($a | $b) - ($a ^ $b);
print($c, "\n");
my $flags = 0;
$flags |= 1;
$flags |= 4;
$flags &= 3;
$flags ^= 1;
print($flags, "\n");
my $x = 255;
my $y = 170;
$x &= $y;
print($x, "\n");
$x |= $y;
print($x, "\n");
$x ^= $y;
print($x, "\n");
# constant bitwise folds
print(0xdeadbeef & 0x0f0f0f0f, "\n");
print(0xdeadbeef | 0x0000ffff, "\n");
print(0xdeadbeef ^ 0xdeadbeef, "\n");
print(~(-1), "\n");
print(~(-0xffff), "\n");
print(~(-123456789), "\n");
# constant-count shifts (unboxed / folded)
print(1 << 0, "\n");
print(1 << 62, "\n");
print(1 << 64, "\n");
print(1 << 100, "\n");
print(1 << -1, "\n");
print(31 >> 1, "\n");
print(31 >> 5, "\n");
print(31 >> 31, "\n");
print(31 >> 63, "\n");
print(31 >> 64, "\n");
print(31 >> -1, "\n");
print(-8 >> 1, "\n");
print(-8 >> 63, "\n");
print(-1 >> 1, "\n");
print(-1 >> 63, "\n");
print((1 << 62) >> 1, "\n");
# variable-count shifts (boxed fallback — pre-existing behavior)
my $n = 3;
my $v = 64;
print($v >> $n, "\n");
print($v << $n, "\n");
my $m = 63;
print(1 >> $m, "\n");
# declaration propagation
my $d = $a & 0xff;
my $e = $a | 0x100;
print($d, $e, "\n");
print(($d & $e) ^ 5, "\n");
# mixed with other I64 ops
print(($a % 7) & ($b % 3), "\n");
# NOTE: `($a + $b) ^ ($a - $b)` (mixed-sign xor) is deliberately absent —
# its result has bit 63 set and real Perl prints it as UV
# (18446744071801622946) while the runtime has no UV storage (prints IV
# negative). Pre-existing gap, out of W1 scope.
print(($a % 7) ^ ($b % 3), "\n");
print(int(($a ^ $b) - ($a | $b)), "\n");
