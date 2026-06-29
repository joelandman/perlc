# if modifier
use 5.010;
say "positive" if 1;
say "never" if 0;

# unless modifier
say "ok" unless 0;
say "never2" unless 1;

# while modifier
my $i = 0;
$i++ while $i < 3;
say $i;   # 3

# until modifier
my $j = 3;
$j-- until $j <= 0;
say $j;   # 0

# for modifier
say $_ for (1, 2, 3);

my @arr = ("a", "b", "c");
say $_ for @arr;

# return with modifier
sub check_pos {
    my ($n) = @_;
    return 0 if $n < 0;
    return $n;
}
say check_pos(5);   # 5
say check_pos(-1);  # 0

# push with modifier
my @out;
my $x = 7;
push @out, $x if $x > 5;
say $out[0];   # 7

# last/next with modifier
foreach my $v (1, 2, 3, 4, 5) {
    next if $v == 3;
    last if $v == 5;
    say $v;   # 1 2 4
}
