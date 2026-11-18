use lib 'tests/lib';
use strict; use warnings;
sub Sub::caller_test {
    use E::IfFlag;
    return enabled();
}
print "enabled in Sub: ", eval { Sub::caller_test() } // "UNDEF", "\n";
print "enabled in main: ", (defined(&enabled) ? "PRESENT" : "ABSENT"), "\n";
print "done\n";
