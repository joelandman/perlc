# W1 (deep): ** constant folding (integer-power range exp 0..30, i64 fit),
# NV-range fallback (exp > 30 or overflow), variable-base ** (boxed),
# abs/int unboxing.
print(2 ** 0, "\n");
print(2 ** 1, "\n");
print(2 ** 10, "\n");
print(2 ** 20, "\n");
print(2 ** 30, "\n");
print(2 ** 31, "\n");
print(2 ** 32, "\n");
print(10 ** 12, "\n");
print(10 ** 15, "\n");
print(10 ** 16, "\n");
print(10 ** 17, "\n");
print(3 ** 19, "\n");
print(3 ** 20, "\n");
print(9 ** 16, "\n");
print(15 ** 15, "\n");
print(16 ** 15, "\n");
print(17 ** 15, "\n");
print(18 ** 15, "\n");
print(19 ** 15, "\n");
print(20 ** 15, "\n");
print((-2) ** 5, "\n");
print((-2) ** 4, "\n");
print(0 ** 0, "\n");
print(0 ** 5, "\n");
print(5 ** 0, "\n");
print(10 ** (-1), "\n");
# variable-base ** (boxed path)
my $base = 3;
my $ex = 7;
print($base ** $ex, "\n");
my $vb = 10;
print($vb ** 2, "\n");
print($vb ** 0.5, "\n");
my $big = 2;
print($big ** 62, "\n");
print($big ** 40, "\n");
# abs / int unboxing on I64 operands
my $neg = -42;
my $pos = 42;
print(abs($neg), "\n");
print(abs($pos), "\n");
print(abs($neg + 5), "\n");
print(int($neg), "\n");
print(int($pos), "\n");
print(int($neg * 3), "\n");
# abs/int on non-int (boxed, unchanged)
print(abs(-1.5), "\n");
print(int(1.9), "\n");
my $z = abs($neg);
print($z, $z + 1, "\n");
my $w = int($pos & 0xff);
print($w, "\n");
