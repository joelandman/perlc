package M;
sub joinargs {
    my $self = shift;
    return join(",", map { ref($_) ? ref($_) : $_ } @_);
}
package main;
my $s = bless {}, "M";
my $row = ["h", "body"];
my %opt = (w => 4, h => 2);
print "row=", $s->joinargs(@$row), "\n";
print "hash=", join(",", sort split /,/, $s->joinargs(%opt)), "\n";
print "mix=", $s->joinargs("A", @$row, 9), "\n";
print "empty=", $s->joinargs(@{[]}), "\n";
my $nested = [["inner", 1], "tail"];
print "nest=", $s->joinargs(@$nested), "\n";
