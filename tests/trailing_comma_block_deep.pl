# D190: trailing comma before `}` on list operators. The comma is the
# last token of the block, so the stored elements are read afterwards.
our @pushed;
sub push_last {
    push @pushed, sprintf("%s-%s", "a", "b"),
}
push_last();
print $pushed[0], "\n";

our @unshifted;
sub unshift_last {
    unshift @unshifted, "u",
}
unshift_last();
print $unshifted[0], "\n";

sub print_last {
    print "p",
}
print_last();
print "\n";

our @rev;
sub reverse_last {
    @rev = reverse 1, 2,
}
reverse_last();
print join(",", @rev), "\n";

our @mapped;
sub map_last {
    @mapped = map { $_ * 2 } 3, 4,
}
map_last();
print join(",", @mapped), "\n";

{
    my @a;
    push(@a, {k => 1});
    print $a[0]{k}, "\n";
}

{
    my @b;
    push @b, "keep", "me";
    print join(",", @b), "\n";
}
