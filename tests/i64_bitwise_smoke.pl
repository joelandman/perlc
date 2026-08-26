# W1 I64 fast path (smoke): safe-IV subset
my $a = 12;
my $b = 10;
print($a & $b, $a | $b, $a ^ $b, "\n");
$a |= 1;
$a &= 15;
print($a, "\n");
print(1 << 4, 32 >> 2, 1 << 64, 1 << -1, "\n");
print(~(-8), ~(-1), ~(-12), "\n");
print(0xdeadbeef & 0x0f0f0f0f, "\n");
