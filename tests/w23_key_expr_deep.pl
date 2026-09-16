my %h = (k => "v", all => "v", keys => "keys-v");
my $k = "K";

# builtin-with-arg in key position (the W23 shapes)
print $h{lc $k}, "\n";
print $h{lc($k)}, "\n";
print $h{uc "k"}, "\n";
print $h{ucfirst "k"}, "\n";
print $h{int(9.9)}, "\n";
print $h{abs -5}, "\n";
print $h{join "", "k"}, "\n";
print $h{length "kk"} || "no-kk", "\n";

# bareword-alone keys stay strings (D136 regression guard)
print "$h{all}\n";
print "$h{keys}\n";
my @a = (); # (kept simple — @{ {..} } deref shape is out of W23 scope)
my $r = {all => 1, sub => 2, and => 3, lc => 4, keys => 5, shift => 6, time => 7};
print "$r->{all} $r->{sub} $r->{and} $r->{lc} $r->{keys} $r->{shift} $r->{time}\n";

# nested builtin-in-key inside expressions
my %c;
my @cmds = ("List", "Get", "Put");
map { $c{uc $_} = 1 } @cmds;
my $n = 0;
$n += $c{$_} for qw(LIST GET);
print "n=$n\n";

# key context inside map with lc
my %m;
map { $m{lc $_} = 1 } ("K");
print "$m{k}\n";

# delete with builtin-computed key
delete $h{lc $k};
print exists $h{k} ? "gone" : "deleted", "\n";

# builtin call as a computed key in a hash-literal pair
my %lit = ((lc "A") => 1, (uc "b") => 2);
print "$lit{a}$lit{B}\n";
# widen: W23 regression guards around the D136 string-key fallback
my %g = (grep => "gv", map => "mv", sort => "sv", reverse => "rv");
print "$g{grep} $g{map} $g{sort} $g{reverse}\n";
my %n = (not => 1, and => 2, or => 3, notkey => 4);
print "$n{not} $n{and} $n{or} $n{not}\n";
# builtin-with-args nested inside a larger key expression
my %c2;
my @w = ("Ab", "Cd");
map { $c2{substr $_, 0, 1} = 1 } @w;
print "c2=$c2{A}$c2{a}\n";
# keyword-alone key inside anon hashref
my $r2 = {lc => 1, uc => 2, keys => 3};
print "$r2->{lc}$r2->{uc}$r2->{keys}\n";
# ternary/paren keys still expressions
my %t = (k => "v");
my $alt = 1;
print $t{$alt ? "k" : "x"}, "\n";
