use strict;

my @a = (10, 20);
sub nums { return (1, 2, 3); }
sub empty { return; }
sub words { return ("echo", "deep"); }

print "A=[", sprintf("%s,%s,%s", nums()), "]\n";
print "B=[", sprintf("%s|%s", @a), "]\n";
print "C=[", sprintf("%s-end", empty()), "]\n";
print "D=[", sprintf("%d+%d=%d", 1, 2, 3), "]\n";

my $t;
open my $fh, '>', \$t or die "open: $!";
printf $fh "F:%s:%s\n", nums(), @a;
close $fh;
print "FH=<$t>\n";

my $p = pack("C*", nums());
print "PACK1=<$p>\n";
my $q = pack("C*", @a);
print "PACK2=<$q>\n";

system(words());
print "deep_done\n";