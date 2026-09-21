use JSON::PP;
print JSON::PP->new->canonical->space_after->encode({a=>1}), "\n";
print JSON::PP->new->allow_nonref->encode("x"), "\n";
eval { JSON::PP->new->allow_nonref(0)->encode("x") };
print "disallow=", ($@ ne "" ? 1 : 0), "\n";
print decode_json('{"a":1,"b":[2]}')->{a}, "\n";
print "done\n";
