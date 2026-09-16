my $x = +5;
print "$x\n";
my $s = "ab";
my $y = +$s;
print "$y\n";
print +(1 + 2), "\n";
my @a = (2,3);
my $n = +$#a;
print "last=$n\n";
sub f { return +7; }
print f(), "\n";
# W19 chain status: use constant B => A + 1 — the injected
# `sub B { return A + 1; }` parses correctly now (unary plus), and the
# main parser resolves A via constMap. `print B` still needs main.cpp to
# seed the throwaway value-parser with constMap (Agent A's file), so B's
# constMap entry holds Call(A,[1]) — noted, not fixable parser-side alone.
use constant K => 5;
use constant K2 => K + 1;
print "K=", K, "\n";
print "injected-sub K2=", K2(), "\n";
