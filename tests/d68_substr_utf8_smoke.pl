# D68: substr UTF-8 awareness smoke test
# Verifies substr correctly handles multi-byte UTF-8 characters
use strict;
use warnings;

# chr(233) = é = UTF-8 bytes C3 A9 (2 bytes, 1 character)
my $s = chr(233) . "bc";
die "D68: length wrong" unless length($s) == 3;
die "D68: ord wrong" unless ord(substr($s, 0, 1)) == 233;
die "D68: ord wrong" unless ord(substr($s, 1, 1)) == 98;
die "D68: ord wrong" unless ord(substr($s, 2, 1)) == 99;
print "ok\n";
