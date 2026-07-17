# D84: % and %= by zero should be eval-catchable exceptions
# The unboxed-int fast path and the boxed path should both
# route through perl_die() instead of exit(1).

# D84a: unboxed-int fast path (inside a sub/scope)
eval {
    my $a = 5;
    my $b = 0;
    my $z = $a % $b;
};
die "FAIL: D84a - should have died" unless $@;
die "FAIL: D84a - wrong message" unless $@ =~ /Illegal modulus zero/;

# D84b: %= by zero
eval {
    my $x = 10;
    $x %= 0;
};
die "FAIL: D84b - should have died" unless $@;
die "FAIL: D84b - wrong message" unless $@ =~ /Illegal modulus zero/;

# D84c: positive % positive (should work)
my $r1 = 7 % 3;
die "FAIL: D84c" unless $r1 == 1;

# D84d: negative % positive (floored semantics)
my $r2 = -7 % 3;
die "FAIL: D84d" unless $r2 == 2;

# D84e: positive % negative (floored semantics)
my $r3 = 7 % -3;
die "FAIL: D84e" unless $r3 == -2;

# D84f: negative % negative (floored semantics)
my $r4 = -7 % -3;
die "FAIL: D84f" unless $r4 == -1;

# D84g: exact multiple
my $r5 = 9 % 3;
die "FAIL: D84g" unless $r5 == 0;

# D84h: zero dividend
my $r6 = 0 % 5;
die "FAIL: D84h" unless $r6 == 0;

print "ok\n";
