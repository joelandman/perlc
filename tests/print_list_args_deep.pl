use strict;

my @a = (10, 20);
my @b = (30, 40);
sub tri { return (1, 2, 3); }
sub empty { return; }

$, = ",";
print "A:", @a, @b, "\n";
print "B:", tri(), 99, "\n";
print "C:", empty(), "end\n";

my $s;
open my $fh, '>', \$s or die "open: $!";
print $fh "F:", @a, tri();
say $fh @b;
print $fh empty(), "z\n";
close $fh;
print "FH=<$s>\n";

$, = "|";
my $t;
open my $fh2, '>', \$t or die "open2: $!";
print $fh2 tri(), @a;
close $fh2;
print "FH2=<$t>\n";

$, = undef;
my $u;
open my $fh3, '>', \$u or die "open3: $!";
my $died = eval {
    print $fh3 "x", die("kaboom"), "y";
    0;
} ? 0 : 1;
close $fh3;
print "DIED=$died OUT=<$u>\n";

my $v;
open my $fh4, '>', \$v or die "open4: $!";
my @d = map { $_ * 2 } @a;
print $fh4 @d, tri();
close $fh4;
print "V=<$v>\n";

print "deep_done\n";