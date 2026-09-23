use Getopt::Std;
@ARGV = qw(-a -b foo rest);
my %o;
getopts("ab:", \%o);
print "a=$o{a} b=$o{b} rest=@ARGV\n";
print "done\n";
