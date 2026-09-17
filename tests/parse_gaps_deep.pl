use strict; use warnings;
# dynamic dispatch with args + SUPER-free plain dispatch
my $obj = bless { v => 5 }, "C";
sub C::get { my ($s, $k) = @_; return $s->{$k}; }
sub C::add { my ($s, $a, $b) = @_; return $a + $b; }
my $m1 = "get"; my $m2 = "add";
print "get=", $obj->$m1("v"), "\n";
print "add=", $obj->$m2(40, 2), "\n";
my @names = ("get", "add");
print "chain=", $obj->$m1("v") + $obj->$m2(1,1), "\n";
# map anon-hash + flatten pairs
my %h = map { $_ => 1 } ("a","b");
print "h=", $h{a}, $h{b}, "\n";
my @p = map { $_ => $_ * 2 } (3);
print "p=@p\n";
my @refs = map { { v => $_ } } (7);
print "refv=", $refs[0]{v}, "\n";
# grep bare named-unary
my @d = grep { defined } (1, undef, 3, undef);
print "d=@d\n";
my @l = grep { length } ("", "ab", "");
print "l=@l\n";
my @rf = grep { ref } ("x", \1, "y");
print "rf=", scalar(@rf), "\n";
my $dd = defined;
print "dd=[", ($dd // ""), "]\n";
# continue blocks
my $t = "";
foreach my $k (1..5) { next if $k == 2; last if $k == 5; } continue { $t .= "c$k,"; }
print "t=$t\n";
my $w = ""; my $n = 0;
while ($n < 4) { $n++; next if $n == 2; } continue { $w .= "w$n,"; }
print "w=$w\n";
my $v = "";
for my $z (1..3) { redo if 0; } continue { $v .= "z$z,"; }
print "v=$v\n";
print "deep_done\n";
