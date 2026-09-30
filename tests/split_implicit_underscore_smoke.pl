$_ = "a/b/c";
my @r = split m{/};
print join(",", @r), "\n";
