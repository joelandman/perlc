use strict;
use warnings;
# D258: `!ref $a and !ref $b` is two calls. `s///` and a one-element
# foreach alias write back through an array element.
my $t = ['See ', ['L', {}, 'W'], '.'];
my $i = 1;
my $merge = (!ref $t->[$i] and !ref $t->[$i - 1]) ? 'Y' : 'N';
print "merge=$merge ref=", ref($t->[$i]), "\n";
my $s = [];
print "bare=", (ref ? 'R' : 'S'), " tern=", (ref $s ? 'R' : 'S'), "\n";
my $a = ['=head1'];
$a->[0] =~ s/^[~=]//;
print "el=[$a->[0]]\n";
my @b = ('=item');
foreach my $line ($b[0]) { $line =~ s/^=//; }
print "al=[$b[0]]\n";
my $para = ['=over', { '~type' => 'text' }];
my $curr = [$para];
print "chain=[", (pop @$curr)->[1]{'~type'}, "]\n";
my $ell = ['L', {}, 'Widget|Widget'];
my $lt = ['Widget'];
@$ell = (@$ell[0, 1], @$lt);
print "t=[$ell->[2]] n=", scalar(@$ell), "\n";
