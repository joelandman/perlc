# D50 smoke test: scalar-ref-rooted autoviv chain (basic case)
my $ref = {};
$ref->{a}{b} = 1;
print "hash_chain: $ref->{a}{b}\n";
print "smoke done\n";
