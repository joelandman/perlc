use strict; use warnings;
my @f = ("a.pl", "b", "c.pl");
foreach my $x (@f) {
    my $y = $x;
    $y =~ s/\.pl$/.pm/ || next;
    print "x=$x y=$y\n";
}
print "mid\n";
my $t = "";
foreach my $q (1..4) {
    my $w = $q * 10 or next;
    $t .= "w$w,";
}
print "t=$t\n";
my $u = "";
foreach my $r (1..3) {
    my $z = $r - 1 and next;
    $u .= "r$r,";
}
print "u=$u\n";
my @g = ("a", "b");
my $cnt = 0;
foreach my $x (@g) {
    $cnt++ unless $x eq "a";
}
print "cnt=$cnt\n";
print "deep_done\n";
