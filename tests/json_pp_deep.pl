use JSON::PP;

# canonical key order (must be forced on both sides for a deterministic
# byte-for-byte compare — real perl's own hash order is randomized
# per-process, so an uncanonical encode_json() output can legitimately
# differ between two runs of real perl itself; see TESTS.md D138 for the
# class of mistake this avoids).
my $enc = JSON::PP->new->canonical->encode({ z => 1, a => 2, m => 3 });
print "canon=$enc\n";

# functional encode_json / decode_json round trip through nested refs
my $data = {
    name   => "Alice",
    age    => 30,
    tags   => ["a", "b", "c"],
    nested => { x => 1, y => [1,2,{z=>3}] },
    active => JSON::PP::true,
    off    => JSON::PP::false,
    extra  => undef,
};
my $j = JSON::PP->new->canonical->encode($data);
print "$j\n";
my $back = decode_json($j);
print "name=$back->{name} age=$back->{age} tags=@{$back->{tags}}\n";
print "nested_x=$back->{nested}{x} nested_y2z=$back->{nested}{y}[2]{z}\n";
print "active=", ($back->{active} ? "yes" : "no"), " active_ref=", ref($back->{active}), "\n";
print "off=", ($back->{off} ? "yes" : "no"), " off_ref=", ref($back->{off}), "\n";
print "extra_defined=", (defined $back->{extra} ? 1 : 0), "\n";

# string escaping round trip: quotes, backslash, control chars
my $s = "line1\nline2\ttab\"quote\\slash";
my $enc2 = JSON::PP->new->canonical->encode({ s => $s });
print "$enc2\n";
print "roundtrip=", (decode_json($enc2)->{s} eq $s ? 1 : 0), "\n";

# top-level scalars and arrays
print encode_json([1, 2, 3.5, "x", undef]), "\n";
print encode_json("just a string"), "\n";
print encode_json(42), "\n";

# empty containers keep their array/hash-ness
print JSON::PP->new->canonical->encode([]), " ", JSON::PP->new->canonical->encode({}), "\n";

# pretty printing
print JSON::PP->new->canonical->pretty->encode({ a => 1, b => [1,2] });

# negative numbers and floats
print JSON::PP->new->canonical->encode({ n => -3.5, i => -7, z => 0 }), "\n";

# decode true/false/null bareword literals directly
print "lit_true=", (decode_json('true') ? 1 : 0), "\n";
print "lit_false=", (decode_json('false') ? 1 : 0), "\n";
print "lit_null_defined=", (defined(decode_json('null')) ? 1 : 0), "\n";

# malformed JSON dies, catchable via eval
eval { decode_json('{not valid') };
print "malformed_died=", ($@ ne "" ? 1 : 0), "\n";

# a cycle in the structure to encode must die, not hang or corrupt
my %cyc;
$cyc{self} = \%cyc;
eval { encode_json(\%cyc) };
print "cycle_died=", ($@ ne "" ? 1 : 0), "\n";

# a DAG (same sub-structure reachable two ways, but not a cycle) is legal
my $shared = { v => 1 };
my $dag = { left => $shared, right => $shared };
print JSON::PP->new->canonical->encode($dag), "\n";

print "done\n";
