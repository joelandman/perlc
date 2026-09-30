use strict;
# Comprehensive adjacent-items test, verifying perlc matches real Perl exactly.

our @a    = (5, 4, 3);
our %h    = (b => 2, a => 1);

# ===== D146: print $H <list-producing function> (sole argument) =====
# The parser must recognize $H as the filehandle when the next token is a
# list-producing function keyword (map/grep/sort/join/keys/values).
# Pre-fix, `$H` was treated as a value argument and stringified to STDOUT,
# leaking `GLOB(0x...)` and leaving the file empty.

my $o;
$o = ""; open my $H, ">", \$o; print $H map  {$_ + 100} (1, 2);  close $H; print "map  : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H grep {$_ > 2} (1..5);   close $H; print "grep : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H sort @a;                close $H; print "sort : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H join(",", @a);          close $H; print "join : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H 1..3;                   close $H; print "range: <$o>\n";
$o = ""; open my $H, ">", \$o; print $H @a;                     close $H; print "arr  : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H reverse @a;             close $H; print "revA : <$o>\n";

# ===== Regression guards: $H is still detected for every existing shape =====
# (These already worked before; they must keep working after the whitelist
# expansion adds KW_ tokens without dropping any existing tokens.)
$o = ""; open my $H, ">", \$o; print $H "literal";                close $H; print "str  : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H 42;                       close $H; print "int  : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H "a"; print $H 7; close $H; print "multi: <$o>\n";
$o = ""; open my $H, ">", \$o; print $H (1, 2, 3);                close $H; print "lit  : <$o>\n";
$o = ""; open my $H, ">", \$o; print $H @a + 100;                 close $H; print "arith: <$o>\n";

# ===== Value-mode (scalar-as-fh) regression guards =====
# Real Perl: `print $V, EXPR` (comma) treats $V as a value. My change must
# not affect this (T2 == COMMA is not in the whitelist, so no change).
my $V = "VAL";
print $V, " after value\n";              # comma: $V is a value
print $V eq "VAL", "\n";                 # cmp op: $V is the LHS value
print $V and 1; print "\n";              # low-prec op: $V is the LHS (D147)

print "adjacent_done\n";
