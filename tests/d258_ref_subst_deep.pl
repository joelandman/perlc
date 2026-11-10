use strict;
use warnings;
# D258 deep: ref's named-unary bound, and in-place s/// on the cell
# a one-element foreach aliases.
my $a = 'ARRAY';
my $b = 'X';
print "dot=[", ref $a . $b, "]\n";
my $x = 1;
print "plus=", defined $x + 1, " refplus=", ref $x + 1, "\n";
$_ = [];
print "q=", (ref ? 'R' : 'S'), " aand=", (ref and 1), "\n";
my $h = { k => ['L'], m => 'text' };
print "hk=", (!ref $h->{k} and !ref $h->{m}) ? 'Y' : 'N', "\n";
my $p = ['=head1', { start_line => 1 }, '  indented'];
$p->[0] =~ s/^[~=]//s;
print "name=[$p->[0]]\n";
my $indent = '  ';
foreach my $line ($p->[2]) {
    $line =~ s/^\Q$indent// if $indent;
}
print "body=[$p->[2]]\n";
my @nums = (10, 20);
foreach my $n ($nums[0]) { $n += 5; }
print "n0=$nums[0] n1=$nums[1]\n";
my $word = 'code';
my $copied = ($word =~ s/c/C/r);
print "r=$copied w=$word\n";
my $cnt = ($p->[0] =~ s/head/HEAD/);
print "cnt=$cnt name2=[$p->[0]]\n";
sub chopw {
    my $width = 10;
    local $_ = "abcdefghij KLMNOP";
    s/^(.{0,$width}) //;
    return $_;
}
print "chop=[", chopw(), "]\n";
my $link = "Widget|Widget";
if ($link =~ m/^([^\|]*)\|(.*)$/s) {
    print "link=[$1] to=[$2]\n";
} else {
    print "link=NO\n";
}
my $currg = [ ['=over', { '~type' => 'text' }] ];
my $over = (grep { $_->[0] eq '=over' } @$currg)[-1];
print "over=[", ($over ? $over->[0] : "NONE"), "]\n";
print "slice=", (10, 20, 30)[1], "\n";
my $para = ['=over', { start_line => 1, '~type' => 'text' }];
my $curr = [$para];
my $ename = 'over-' . ( (pop @$curr)->[1]{'~type'} );
print "end=[$ename]\n";
my $multi = [10, 20, 30];
my $tick = 0;
sub bump { $tick++; return 7 }
print "lastarrow=", ((bump(), $multi)->[1]), " tick=$tick\n";
my $ell = ['L', { type => 'old' }, 'Widget|Widget'];
my $link_text = ['See', 'Widget'];
@$ell = (@$ell[0, 1], @$link_text);
print "kids=", scalar(@$ell), " t0=[$ell->[2]] t1=[$ell->[3]]\n";
my $e2 = ['A', 'B', 'C'];
@$e2 = (@$e2[0, 1], 'Z');
print "e2=", join('|', @$e2), "\n";
my $e3 = ['P', 'Q'];
$e3->@* = ('P', 'R');
print "e3=", join('|', @$e3), "\n";
