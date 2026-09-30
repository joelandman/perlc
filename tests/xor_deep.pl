# D149 deep: low-precedence logical xor (was compiled as ||), plus
# `A or B and C` inside an expression (was a parse error).
use strict; use warnings;
my @v = (0, 1, "", "a", "0", undef, 2.5, "0.0");
for my $x (@v) { for my $y (@v) {
    my $r = ($x xor $y);
    print defined $x ? "[$x]" : "[u]", defined $y ? "[$y]" : "[u]", "=<", $r, ">\n";
} }
my $n = 0;
sub t { $n++; return $_[0] }
my $r = (t(1) xor t(1)); print "both-evaluated n=$n r=<$r>\n";
$r = (t(0) xor t(1) xor t(1)); print "chain n=$n r=<$r>\n";
$r = (0 or 1 xor 1); print "or-xor r=<$r>\n";
$r = (1 xor 0 and 0); print "xor-and r=<$r>\n";
print "ok\n" if 1 xor 0;
print "no\n" if 1 xor 1;
print "unl\n" unless 1 xor 1;
my @a = (1) x 3;
if (@a xor 0) { print "array-cond\n" }
my $s = (1 xor 0) ? "T" : "F"; print "$s\n";
$r = !!(1 xor 0); print "not-not <$r>\n";
my $x = 5; ($x > 3 xor $x < 10) or print "neither-or-both\n";
my $z = (open(my $fh, "<", "/nonexistent") xor print "xor-stmt\n"); print "z=<$z>\n";
$r = (1 or 0 and 0); print "or-and1 <$r>\n";
$r = (0 or 1 and 0); print "or-and2 <$r>\n";
$r = (0 or 0 or 1 and 2); print "or-and3 <$r>\n";
$r = (0 xor 1 and 1); print "xor-and <$r>\n";
my @l = grep { $_ % 2 xor $_ > 4 } 1 .. 8; print "grep <@l>\n";
