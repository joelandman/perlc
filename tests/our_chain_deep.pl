# D191: chained our/my, fat-arrow and hash-key barewords stay strings,
# and statement-level `our ... or die` still does not die.
our $a = our $b = our $c = 7;
print "$a|$b|$c\n";

my $x = my $y = 4;
print "$x $y\n";

my %h = (our => 1, my => 2);
$h{our} = 3;
print "$h{our}|$h{my}\n";

our $z = 1 or die "should-not-die";
print "$z\n";

our $p = 0;
for (our $i = our $j = 0; $i < 2; $i++) {
    $p += $i + $j;
}
print "$p|$i|$j\n";

sub nest {
    our $n = our $m = 9;
    return "$n$m";
}
print nest(), "\n";

print our $q = 5;
print "\n";
