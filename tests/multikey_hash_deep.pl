# Multi-key hash subscript, fixed in dae3c35 (parseHashKeySingle).
# $h{"a","b"} -- real Perl's multi-dimensional-
# emulation hash key, a comma-separated list inside a single
# (non-slice) hash subscript joined into one composite key with the
# $; separator (default "\x1c", SUBSEP) -- was a hard "expected } but
# got ','" parse error. None of the ~13 single-key subscript parse
# sites (plain $h{key}, $ref->{key}, $$ref{key}, ${EXPR}{key}, exists/
# delete, local) ever consumed a comma; only the separate slice sites
# (@h{...}) already handled commas, with different (correct, not to
# be touched) semantics. Found via a real
# /usr/bin/callgrind_annotate script: `$compressed{$context,$index}`.

my %compressed;
$compressed{"ctx","idx"} = "realname";
print "basic=", $compressed{"ctx","idx"}, "\n";

# three-key
my %tri;
$tri{"a","b","c"} = 99;
print "threekey=", $tri{"a","b","c"}, "\n";

# different first-component values must NOT collide (confirms the
# join is actually keying on BOTH components, not just concatenating
# without a separator)
my %collide;
$collide{"a","bc"} = 1;
$collide{"ab","c"} = 2;
print "nocollide1=", $collide{"a","bc"}, "\n";
print "nocollide2=", $collide{"ab","c"}, "\n";

# regression: plain single-key hash elem
my %h = (x => 1);
print "plain=", $h{x}, "\n";

# regression: arrow deref single key
my $href = { y => 2 };
print "arrow=", $href->{y}, "\n";

# regression: $$href{key} sigil chain (D63)
print "sigilchain=", $$href{y}, "\n";

# regression: ${$href}{key} symbolic deref subscript (D188)
print "symbolicderef=", ${$href}{y}, "\n";

# regression: exists/delete single key
print "exists_before=", (exists($h{x}) ? "yes" : "no"), "\n";
delete $h{x};
print "exists_after=", (exists($h{x}) ? "yes" : "no"), "\n";

# regression: local $ref->{key} (hash_elem_deref)
sub f {
    my $r = { z => 1 };
    local $r->{z} = 5;
    print "localderef=$r->{z}\n";
}
f();

# regression: hash slice (@h{...}) -- a DIFFERENT node, must be
# completely unaffected by the single-key comma-join fix
my %s = (a=>1,b=>2,c=>3);
my @vals = @s{qw(a b)};
print "slice=@vals\n";

print "done\n";
