use strict;
use warnings;
# D254: require Encode finds archlib without an explicit -I.
my $r = eval { require Encode };
print "r=", defined($r) ? $r : "U", "\n";
my $m = eval { require NoSuchModuleD254 };
print "miss=", defined($m) ? $m : "U", "\n";
