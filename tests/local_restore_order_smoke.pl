sub f {
    my $r = { z => 1 };
    local $r->{z} = 5;
    print "$r->{z}\n";
}
f();
my %s = (a => 1, b => 2);
my @vals = @s{qw(a b)};
print "@vals\n";
