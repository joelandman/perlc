use Getopt::Long qw(GetOptions);
Getopt::Long::config('bundling');
my %o;
@ARGV = ('--help');
my $ok = GetOptions(\%o, 'help|h');
print "ok=", ($ok ? 1 : 0), " help=", ($o{help} ? 1 : 0), "\n";
print "done\n";
