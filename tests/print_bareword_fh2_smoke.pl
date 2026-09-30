# D155: `print WORD;` / `print WORD g()` — WORD is a filehandle unless it
# names a sub (was: printed the literal word / called nothing).
sub g { "G" }
sub h { "H(@_)" }
sub NAMED { "named" }
$_ = "dflt";
print NOPE;
print NOPE g();
print NOPE if 1;
print h g();
print "\n";
print NAMED;
print "\n";
my $n = 0;
sub f { $n++; "v" }
print NOPE f();
print "n=$n\n";
my $t = "/tmp/perlc_pbfh2_$$";
sub opener { open(LOG, ">", $t) or die }
opener();
$_ = "from-underscore";
print LOG;
print LOG g();
close LOG;
open(my $in, "<", $t) or die;
print "file=[", <$in>, "]\n";
close $in;
unlink $t;
