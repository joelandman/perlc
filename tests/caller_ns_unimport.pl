use lib 'tests/lib';
use strict;
use warnings;

use E::NixFoo;
print "after use:    ", (defined(&foo) ? 'PRESENT' : 'ABSENT'), "\n";
print "foo() = ", foo(), "\n";

no E::NixFoo qw(foo);
print "after no:     ", (defined(&foo) ? 'PRESENT' : 'ABSENT'), "\n";
print "done\n";
