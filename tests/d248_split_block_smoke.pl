my @a = map { split m%\s*/\s*% } ("a/b", "c");
print join("|", @a), "\n";
my @b = grep { split /x/ } ("x", "y");
print scalar(@b), "\n";
