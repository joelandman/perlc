package Point;
sub new {
    my ($class, $x, $y) = @_;
    bless [ $x, $y ], $class;
}
sub DESTROY {
    my ($self) = @_;
    print "D:", $self->[0], ":", $self->[1], "\n";
}

package Box;
sub new {
    my ($class, @items) = @_;
    bless [ @items ], $class;
}
sub DESTROY {
    my ($self) = @_;
    print "B:", join(",", @$self), "\n";
}

package main;
my $p = Point->new(1, 2);
my $q = $p;
$p = undef;
print "A\n";
$q = undef;
print "B\n";

my @holders;
for my $i (1..3) {
    my $box = Box->new($i, $i + 10);
    $holders[$i - 1] = $box;
}
@holders = ();
print "C\n";

my $r = Point->new(7, 8);
$r = Point->new(9, 10);
$r = undef;
print "D\n";
print "deep_done\n";