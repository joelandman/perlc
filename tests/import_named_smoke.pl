use lib 'tests/lib';
use E::NamedOne;
print "one=", eval { one() } // "FAIL", "\n";
print "smoke_done\n";
