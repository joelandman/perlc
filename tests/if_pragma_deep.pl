use if 1, "constant", YES => 1;
use if 0, "constant", NO => 2;
print "yes=", YES(), "\n";
use if 1, "Getopt::Std";
@ARGV = qw(-x);
my %o;
getopts("x", \%o);
print "x=$o{x}\n";
print "done\n";
