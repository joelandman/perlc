# D266: parenthesized my-handle, the older my $dh form, and a failed open.
use strict;
use warnings;
my $dir = ".";
opendir my ($dh), $dir or die "open: $!";
my %seen;
while (my $ent = readdir $dh) { $seen{$ent} = 1; }
closedir $dh;
print "dot=", ($seen{"."} ? 1 : 0), " dotdot=", ($seen{".."} ? 1 : 0), "\n";
opendir(my ($dh2), $dir) or die "open2: $!";
my $n2 = 0;
$n2++ while readdir $dh2;
closedir $dh2;
print "paren_call=", ($n2 > 0 ? 1 : 0), "\n";
opendir my $dh3, $dir or die "open3: $!";
my $n3 = 0;
$n3++ while readdir $dh3;
closedir $dh3;
print "plain=", ($n3 == $n2 ? 1 : 0), "\n";
my $bad = opendir my ($dh4), "this-dir-does-not-exist-d266";
print "bad=", ($bad ? 1 : 0), "\n";
