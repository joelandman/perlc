D196: indirect array last-index — #$ref / $#{$ref}, deep coverage.
my @flat = (1, 2, 3, 4, 5);
my $flatref = \@flat;
my @empty = ();
my $emptyref = \@empty;

# direct ($) vs braces ({}) forms agree
my $a = $#$flatref;
my $b = $#{$flatref};
print "direct=$a braces=$b flat_last=4\n";

# empty array last index is -1 in real Perl
print "empty=$#$emptyref\n";

# through a hash element that holds an array ref
my %h = (list => [10, 20, 30, 40]);
print "hash_elem=$#{$h{list}}\n";

# through an array element that holds an array ref
my @cells = ([7, 8], [9]);
print "cell0=$#{$cells[0]} cell1=$#{$cells[1]}\n";

# as a for-loop bound (the ckbcomp / linux-update-symlinks shapes)
my $sum = 0;
for my $i (0 .. $#$flatref) { $sum += $flat[$i]; }
print "sum=$sum\n";

# in arithmetic / comparison / list context
print "arith=", (($#$flatref) * 2 + 1) . "\n";
print "cmp=", ($#$flatref == 4 ? "eq" : "ne"), "\n";
my @slice = @flat[0 .. $#$flatref];
print "slice=@slice\n";

# interpolation with surrounding literal text
my $s = "last=$#$flatref total=" . (@flat) . "\n";
print $s;

# named form ($#arr) still works
my @named = (100, 200);
print "named=$#named\n";
