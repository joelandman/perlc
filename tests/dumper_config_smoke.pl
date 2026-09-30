# D155 smoke: Data::Dumper honours $Data::Dumper::Indent/Terse (were
# ignored) and prints integer literals unquoted (`[1,2]` used to dump as
# '1','2' because the compact FLOAT_PAIR/FLAT_ARRAY storage lost IV-ness).
use Data::Dumper;
$Data::Dumper::Sortkeys = 1;
print Dumper([1, 2], {a => 3});
{ local $Data::Dumper::Indent = 1; print Dumper({x => [1, "y"]}); }
{ local $Data::Dumper::Terse = 1; print Dumper([4, 5]); }
{ local $Data::Dumper::Indent = 0; print Dumper({k => [6, 7]}), "\n"; }
