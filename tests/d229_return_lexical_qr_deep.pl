use strict;
use warnings;

sub grab {
    my ($pat, $ok) = @_;
    my $re = eval "qr/$pat/";
    return "bad" if $@;
    return "" unless $ok;
    return $re;
}

sub word {
    my $s = shift;
    return $s;
}

sub num {
    my $i = 4;
    return $i;
}

our $g;
sub with_local {
    local $g = 3;
    my $s = "z";
    return $s;
}

my $re = grab('[[:^ascii:]]', 1);
print "def=", (defined $re ? 1 : 0), "\n";
print "len=", length($re), "\n";
print "s=$re\n";
my $fallback = "no";
$fallback = qr/[\x80-\xFF]/ unless $re;
print "fb=$fallback\n";

my $empty = grab('a', 0);
print "empty=[$empty]\n";

print "w=", word("hi"), "\n";
print "i=", num(), "\n";
print "l=", with_local(), " g=", (defined $g ? $g : "U"), "\n";

my $re2 = grab('x+', 1);
print "m=", ("xxx" =~ $re2 ? 1 : 0), "\n";
print "s2=$re2\n";
