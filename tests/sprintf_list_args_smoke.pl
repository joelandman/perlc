use strict;

sub nums { return (1, 2); }
sub words { return ("echo", "smoke"); }

my $s = sprintf("%s-%s\n", nums());
print "A=<$s>\n";

my $t;
open my $fh, '>', \$t or die "open: $!";
printf $fh "%s|%s\n", nums();
close $fh;
print "B=<$t>\n";

system(words());
print "smoke_done\n";