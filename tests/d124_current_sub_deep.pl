use feature 'current_sub';
sub make {
    my $add = shift;
    my $f = sub { my $n = shift; return $n <= 0 ? 0 : $add + __SUB__->($n - 1); };
    return $f;
}
my $f1 = make(10); my $f2 = make(100);
print "cap10: ", $f1->(3), "\n";
print "cap100: ", $f2->(3), "\n";
my $counter = 0;
my $acc = sub { my $n = shift; $counter += $n; return $n > 0 ? __SUB__->($n - 1) : $counter; };
print "acc: ", $acc->(3), " counter: ", $counter, "\n";
print "outside: ", defined(__SUB__) ? 1 : 0, "\n";
sub in_named { return __SUB__ == \&in_named ? "same" : "diff"; }
print "named: ", in_named(), "\n";
my $deep;
$deep = sub { my $n = shift; return 0 if !$n; return 1 + __SUB__->($n - 1); };
print "deep: ", $deep->(50), "\n";
my $nested = { r => sub { my $n = shift; return 1 if $n == 1; return 2 * __SUB__->($n - 1); } };
print "nested: ", $nested->{r}->(5), "\n";
my $list = sub { return (1, __SUB__->($_[0] - 1)) if $_[0] > 1; return (1); };
my @l = $list->(3);
print "list: @l\n";
print "end\n";
