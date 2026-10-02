my @versions = ([1,"a"],[3,"c"],[2,"b"]);
my @sorted = sort({ $a->[0] <=> $b->[0] } @versions);
print join(",", map { $_->[1] } @sorted), "\n";
