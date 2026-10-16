# D192: override, brand-new name, closure capture, *{EXPR} = \&{EXPR},
# and the string form of a computed typeglob.
sub hello { print "hi\n" }
my $n = "hello";
*$n = sub { print "dyn\n" };
hello();

my $m = "fresh";
*$m = sub { print "new\n" };
fresh();

my $cap = "cap";
my $s = "show";
*$s = sub { print "$cap\n" };
show();
$cap = "cap2";
show();

sub orig { print "orig\n" }
my $a = "kept";
my $b = "orig";
*{$a} = \&{$b};
kept();

print *$n, "\n";

my $x = "aliased";
my $g = "slot";
*$g = \$x;
$x = "changed";
print ${"slot"}, "\n";
