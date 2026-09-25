use strict;

my @a = (1, 2);
sub f { return (3, 4); }

$, = ",";
print f(), "x\n";
print @a, 9, "\n";

my $s;
open my $fh, '>', \$s or die "open: $!";
print $fh f(), 5, "\n";
say $fh @a;
close $fh;
print "FH=<$s>\n";

$, = undef;
my $out;
open my $fh2, '>', \$out or die "open2: $!";
my $died = eval {
    print $fh2 "a", die("boom"), "b";
    0;
} ? 0 : 1;
close $fh2;
print "DIED=$died OUT=<$out>\n";
print "smoke_done\n";