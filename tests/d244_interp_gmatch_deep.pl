my $text = "NAME";
my $sp = " ";
my $n = 0;
my @got;
while ($text =~ m/\G(?:([A-Z])|($sp))/xg) {
    $n++;
    push @got, defined $1 ? $1 : (defined $2 ? "S" : "?");
    last if $n > 10;
}
print "n=$n got=", join(",", @got), "\n";
my $p = pos($text);
print "pos=", (defined $p ? $p : "U"), "\n";
my $s = "zz";
my $once = 0;
while ($s =~ m/$s/g) {
    $once++;
    last if $once > 3;
}
print "self=$once pos=", (defined pos($s) ? pos($s) : "U"), "\n";
