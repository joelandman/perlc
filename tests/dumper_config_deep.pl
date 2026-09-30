# D155 deep: Data::Dumper configuration globals — Indent 0/1/2, Terse,
# Useqq, Quotekeys, Pair, Varname, Trailingcomma, Sortkeys — plus IV/NV
# fidelity for compact numeric literals.
use Data::Dumper;
my $d = {a=>1, b=>[1,"x",2.5], c=>{d=>undef}};
$Data::Dumper::Sortkeys=1;
for my $i (0,1,2) { for my $t (0,1) { local $Data::Dumper::Indent=$i; local $Data::Dumper::Terse=$t; print "--- Indent=$i Terse=$t\n", Dumper($d, 5); } }
{ local $Data::Dumper::Terse=1; print Dumper("s"), Dumper([]), Dumper({}); }
{ local $Data::Dumper::Indent=1; print Dumper(bless({x=>[1]}, "Foo")); }
{ local $Data::Dumper::Quotekeys=0; print Dumper({abc=>1, "a b"=>2, 12=>3, "01"=>4, "-x"=>5, "-12"=>6, "-0"=>7, ""=>8, "Foo::Bar"=>9}); }
{ local $Data::Dumper::Pair=": "; local $Data::Dumper::Varname="X"; print Dumper({k=>1}); }
{ local $Data::Dumper::Trailingcomma=1; print Dumper([1,2]); local $Data::Dumper::Indent=0; print Dumper([1,2]), "\n"; }
{ local $Data::Dumper::Useqq=1; print Dumper("a\tb\n\x{1}\$x\@y\"z\x27", 2.5, 7, "5", "x\x{7f}1\x{01}2", {"k\n"=>1}); }
{ local $Data::Dumper::Indent=1; print Dumper(\ [1]); local $Data::Dumper::Terse=1; print Dumper([[1,[2]],{}]); }
print Dumper(\"s", \\1);
$Data::Dumper::Indent = 1; print Dumper([1]); $Data::Dumper::Indent = 2;
sub inner { return Dumper({z=>1, y=>2}) } print inner();
# numeric literal fidelity
print Dumper([1, 2], [3, 4.5], [5, 6, 7], [-1, 0]);
my @rows = ([1, 2, 3], [4, 5, 6]);
print Dumper(\@rows);
my $pair = [1, 2];
my $copy = $pair;
push @$copy, 3;
print Dumper($pair);
my $e = [7, 8]; my $v = $e->[0]; print Dumper($v);
