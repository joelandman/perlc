use strict;
use warnings;

sub grab {
    my ($pat) = @_;
    my $re = eval "qr/$pat/";
    return "" if $@;
    return $re;
}

my $re = grab('a.c');
print "m=", ("abc" =~ $re ? 1 : 0), "\n";
print "s=$re\n";
my $n = 0;
$n = 7 unless $re;
print "n=$n\n";
