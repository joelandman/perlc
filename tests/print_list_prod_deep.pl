use strict;

# print_list_prod_deep — deeper coverage of list-producing expressions as
# print arguments (see print_list_prod_smoke.pl for the headline bug and
# the fix). Adds: mixed scalar+list arguments, a custom-comparator sort,
# a sub that returns its own list printed directly, and $, interaction.
# Verified byte-for-byte against real Perl 5.42; `print` stdout only.

our @a    = (2, 3, 5, 7);
our %long = (d => 4, b => 2, a => 1, c => 3);

# (A) every supported list producer, sole-argument.
print "map    = ", map  {$_ * 3}  (1, 2, 3);
print "map2   = ", map  {$_ + 100} @a;
print "grep   = ", grep {$_ > 2}  @a;
print "grep0  = ", grep {$_ > 99} @a;         # empty list: prints nothing
print "sort   = ", sort @a;
print "sdesc  = ", sort {$b <=> $a} @a;       # custom comparator
print "range  = ", 2..6;
print "array  = ", @a;
print "keys   = ", sort keys   %long;
print "vals   = ", sort values %long;
print "\n";

# (B) leading label + list producer (list producer is the last argument so
#     the greedy map/grep forms consume nothing after them).
print "c-map  = x",  map  {$_ * 10} (1, 2, 3);
print "c-grep = x",  grep {$_ % 2}  (1..6);
print "c-sort = x",  sort @a;
print "c-range= x",  1..3;
print "c-arr  = x",  @a;
print "\n";

# (C) non-greedy list producers between scalars (these expand as a single
#     list argument and do not consume their neighbours).
print "m-arr  = ", "p", @a, "q";
print "m-sort = ", "p", sort @a, "q";
print "m-range= ", "p", 1..3, "q";
print "m-ks   = ", "p", sort keys   %long, "q";
print "m-vs   = ", "p", sort values %long, "q";
print "\n";

# (D) a sub that returns a list, printed directly (wantarray/list-context
#     path for user-defined sub calls as a print argument).
sub triples { my @r = (7, 8, 9); return @r; }
sub bigs    { return sort (5, 1, 4, 2, 3); }
print "sub    = ", triples();
print "subL   = x", triples(), "y";
print "subS   = ", bigs();

# (E) sprintf also expands list-producing arguments via the same
#     isExplicitListKind predicate (previously it too collapsed to a count).
print "spr-arr  = ", sprintf("%d|%d|%d", @a), "\n";
print "spr-map  = ", sprintf("%d|%d|%d", map  {$_ * 10} (1, 2, 3)), "\n";
print "spr-sort = ", sprintf("%d|%d|%d", sort @a), "\n";
print "spr-range= ", sprintf("%d|%d|%d", 1..3), "\n";

# (F) regression guards (previously-correct paths must stay correct after
#     the isExplicitListKind change).
print "join   = ", join("|", map {$_ * 3} (1, 2, 3)), "\n";
print "sarr   = N (elemcount=", scalar(@a), ")\n";
print "deep_done\n";
