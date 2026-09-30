use strict;

# print_list_prod_smoke — list-producing expressions as print arguments.
#
# Real Perl evaluates each print argument in list context, so a list
# producer (`map`, `grep`, `sort`, `1..N`, `@arr`) prints its ELEMENTS.
# perlc collapsed such an argument to scalar context and printed the
# element COUNT instead (or undef for `sort`) — silent wrong data:
#   print map {$_*10} (1,2,3);   # perl: 102030   perlc(was): 3
#   print sort @a;               # perl: 123      perlc(was): ""
#
# The fix is codegen: the print/say argument-collection branches now
# recognise these unambiguously-list-producing kinds and expand them via
# emitArrayPtr, exactly the way they already treated `@arr`.
# Verified byte-for-byte against real Perl 5.42. Only `print` stdout forms
# are exercised here (the in-memory-filehandle and bare-`say` paths are
# covered/claimed separately and are not part of this fix).

our @a = (3, 1, 2);
our %h = (b => 2, a => 1);

# (A) sole-argument list producers — the core shape.
print "map   : "; print map  {$_ * 10} (1, 2, 3);
print "grep  : "; print grep {$_ > 1}  (1..5);
print "sort  : "; print sort @a;
print "range : "; print 1..3;
print "array : "; print @a;
print "keys  : "; print sort keys   %h;   # sorted: order pinned across runtimes
print "vals  : "; print sort values %h;

# (B) leading label scalar, list producer last (greedy-safe: map/grep are
#     the final argument, so there is nothing after them to consume).
print "b-map  : "; print "x" , map  {$_ * 10} (1, 2);
print "b-sort : "; print "x" , sort @a;
print "b-range: "; print "x" , 1..3;
print "b-arr  : "; print  "x" , @a;

# (C) regression guards — these list producers were already correct before
#     the fix and must stay so.
print "join   : ", join("|", map {$_ * 10} (1, 2, 3)), "\n";
print "scalar : ", scalar(map {$_ * 10} (1, 2, 3)), "\n";
print "smoke_done\n";
