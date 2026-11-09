my $para = "ab";
my $x = "a";
my $n = 0;
while ($para =~ m/\G$x/g) {
    $n++;
    $x = "b" if $n == 1;
    last if $n > 5;
}
my $p = pos($para);
print "n=$n pos=", (defined $p ? $p : "U"), "\n";
