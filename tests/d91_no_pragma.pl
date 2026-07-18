# D48/D91: no PRAGMA forms should parse silently as no-ops
use warnings;

# These all should parse without error
no strict;
no warnings;
no warnings "substr";
no warnings "uninitialized";

# Verify program runs correctly after no pragmas
my $x = 42;
print $x, "\n";
print "ok\n";
