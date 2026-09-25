use strict; use warnings;
# read-all + empty + append + seek + last-index
my $e = "";
open(my $f0, '<', \$e) or die "no0";
my @none = <$f0>;
close $f0;
print "none=", scalar(@none), "\n";
my $s = "x\ny\nz\n";
open(my $fh, '<', \$s) or die "no1";
my @all = <$fh>;
close $fh;
print "all=", scalar(@all), " last=[$all[-1]]\n";
my $t = "keep\n";
open(my $fh2, '>>', \$t) or die "no2: $!";
print $fh2 "added\n";
close $fh2;
print "t=[$t]\n";
open(my $fh3, '+<', \$s) or die "no3: $!";
my $first = <$fh3>;
print $fh3 "X\n";
close $fh3;
print "first=[$first] s=[$s]\n";
my $u = "";
open(my $fh4, '<', \$u) or die "no4: $!";
my $r = <$fh4>;
print "emptystr=[", ($r // "undef"), "]\n";
close $fh4;
open(my $fh5, '<', \$s) or die "no5";
seek($fh5, 2, 0);
my $mid = <$fh5>;
close $fh5;
print "mid=[$mid]\n";
# slurp via $/ = undef
my $b = "1\n2\n";
open(my $fh6, '<', \$b) or die "no6";
local $/; my $slurp = <$fh6>;
close $fh6;
print "slurp=[$slurp]\n";
# writing to an in-memory filehandle whose backing scalar is still undef
# must turn that scalar into a string (real Perl semantics)
my $v;
open(my $fh7, '>', \$v) or die "no7";
print $fh7 "p\n";
close $fh7;
print "v=[$v]\n";
print "deep_done\n";
