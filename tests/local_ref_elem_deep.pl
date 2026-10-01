# Deep test for D169: `local $ref->{key} = val;` / `local $ref->[idx]
# = val;` (localizing a hash/array element reached through a scalar
# reference, not a named hash/array) was a hard "unexpected token
# '->'" parse error. Found via a real
# /usr/share/doc/libdbi-perl/examples/perl_dbi_nulls_test.pl script's
# common DBI idiom: `local $dbh->{PrintError}=0;` (temporarily
# suppressing DBI error reporting inside a block/sub, restored on
# scope exit).
my %h = (a => 1, b => 2);
my $href = \%h;

# basic localize + restore on block exit
{
    local $href->{a} = 99;
    print "block_inside=$h{a}\n";
}
print "block_outside=$h{a}\n";

# restore on SUB exit (the real-world DBI shape: a helper sub
# localizes a setting for the duration of its own call, callees see
# the localized value, caller sees the original again afterward)
sub inner { return "inner_sees=$h{a}"; }
sub outer_with_local {
    local $href->{a} = "localized";
    return inner();
}
print outer_with_local(), "\n";
print "after_sub=$h{a}\n";

# array element via ref
my @arr = (10, 20, 30);
my $aref = \@arr;
{
    local $aref->[1] = 999;
    print "arr_inside=$arr[1]\n";
}
print "arr_outside=$arr[1]\n";

# bare `local $ref->{key};` with no assignment — real Perl assigns
# undef (same convention as bare `local $h{k};`)
{
    local $href->{b};
    print "bare_local_defined=", (defined($h{b}) ? "yes" : "no"), "\n";
}
print "bare_local_restored=$h{b}\n";

# restoration survives a die/eval unwind through the localized scope
# (real Perl's local restoration is a true dynamic-scope unwind, not
# just a block-exit special case)
eval {
    local $href->{a} = "about_to_die";
    die "boom\n";
};
print "after_eval_die=$h{a}\n";

# regression: the pre-existing named-hash/array local forms must be
# completely unaffected
my %named = (k => 1);
{
    local $named{k} = 42;
    print "named_hash_inside=$named{k}\n";
}
print "named_hash_outside=$named{k}\n";

my @namedarr = (1,2,3);
{
    local $namedarr[0] = 100;
    print "named_arr_inside=$namedarr[0]\n";
}
print "named_arr_outside=$namedarr[0]\n";

print "done\n";
