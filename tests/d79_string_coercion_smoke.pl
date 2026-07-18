# D79: String-to-number coercion - no hex auto-detect (smoke test)
# Implicit coercion is purely decimal.

my $a = "0x10" + 1;
die unless $a == 1;

my $b = "077" + 1;
die unless $b == 78;

my $c = "0b1010" + 1;
die unless $c == 1;

my $d = "123abc" + 1;
die unless $d == 124;

print "ok\n";
