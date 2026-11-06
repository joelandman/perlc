use strict;
use warnings;

# D197 (fixed 2026-11-06): `defined($param)` lied for missing @_ args when
# the param was int-promoted by the Stage-25 ladder (the undef sentinel
# numerified to 0 boxed an INT, which is always defined). The ladder's
# floatSafe gate now refuses promotion when the body observes defined()
# (including inside a ternary condition — the walk previously never
# reached the ternary's cond operand at all). Params that never observe
# definedness keep promoting exactly as before.

sub m2 { my ($y) = @_; return defined($y) ? "D" : "U"; }
print "m2: ", m2(), m2(1), "\n";

sub m3 { my ($x, $y) = @_; return defined($y) ? "Dy" : "Uy"; }
print "m3: ", m3(5), "|", m3(5, 6), "\n";

sub m4 { my ($z) = @_; if (defined($z)) { return "Dz"; } return "Uz"; }
print "m4: ", m4(), m4(9), "\n";

sub m5 { my ($w) = @_; my $r = (defined($w)) ? "d" : "u"; return $r; }
print "m5: ", m5(), "|", m5(3), "\n";
