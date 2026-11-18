package Sub;
use lib 'tests/lib';
use strict;
use warnings;

use E::NixFoo;
print "in Sub (before no): ", (defined(&foo) ? 'PRESENT' : 'ABSENT'), "\n";

no E::NixFoo qw(foo);
print "in Sub (after no):  ", (defined(&foo) ? 'PRESENT' : 'ABSENT'), "\n";

print "done\n";
