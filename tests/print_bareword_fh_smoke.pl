# D153 smoke: print/say/printf to a bareword filehandle that perlc has not
# seen opened (`print NOPE "x"`) was a compile error ("String found where
# operator expected"); perl reads NOPE as a filehandle and prints nothing.
# Also: printf to a bareword handle used to go to STDOUT.
use feature 'say';
sub foo { "F(@_)" }
print NOPE "x";
print NOPE "a", "b";
say NOPE "y";
printf NOPE "%s", 1;
print foo "arg";          # a declared sub is still a call
print "\n";
my $t = "/tmp/perlc_pbfh_smoke_$$";
open(LOG, ">", $t) or die;
printf LOG "%03d|", 7;
close LOG;
open(my $in, "<", $t) or die;
print "file=[", scalar(<$in>), "]\n";
unlink $t;
