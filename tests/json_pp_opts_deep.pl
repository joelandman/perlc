use JSON::PP;

print "space=", JSON::PP->new->canonical->space_before->space_after->encode({a=>1}), "\n";
print "pretty=\n", JSON::PP->new->canonical->pretty->encode({a=>1,b=>[1,2]});

eval { JSON::PP->new->allow_nonref(0)->encode("no") };
print "oo_disallow=", ($@ =~ /allow_nonref/ ? 1 : 0), "\n";
print "oo_allow=", JSON::PP->new->allow_nonref->encode(42), "\n";

print "from=", decode_json('{"a":2}')->{a}, "\n";

my $u = decode_json('"\\u00e9"');
print "u_len=", length($u), "\n";

{
    package T;
    sub TO_JSON { {ok => 1} }
}
my $o = bless {}, "T";
print "blessed=", JSON::PP->new->convert_blessed->canonical->encode($o), "\n";

print "done\n";
