use strict;
use Getopt::Long qw(GetOptions);

Getopt::Long::config('bundling');
my %o;
@ARGV = ('--name=Foo', '--section=3');
my $ok = GetOptions(\%o, 'name|n=s', 'section|s=s', 'help|h', 'utf8|u');
print "ok=", ($ok ? 1 : 0), "\n";
print "name=", $o{name}, " section=", $o{section}, "\n";
print "left=", join(",", @ARGV), "\n";

Getopt::Long::config('bundling_override');
@ARGV = ();
my %p;
my $ok2 = GetOptions(\%p, 'alt|a', 'width|w=i');
print "ok2=", ($ok2 ? 1 : 0), " alt=", (defined $p{alt} ? 1 : 0), "\n";
print "done\n";
