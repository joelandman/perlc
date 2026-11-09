sub entries {
    my ($self) = @_;
    my @items = map { split m%\s*/\s*% } @{ $$self{INDEX} };
    return @items;
}
my $s = { INDEX => ["a/b", "c / d"] };
print join(",", entries($s)), "\n";
my @z = map { split /:/, $_, 2 } ("p:q:r", "s");
print join("|", @z), "\n";
