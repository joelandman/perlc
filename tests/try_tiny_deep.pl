use Try::Tiny;

my $fin = 0;
&try(sub {
    die "boom";
}, &catch(sub {
    print "err_has_boom=", ($_ =~ /boom/ ? 1 : 0), "\n";
}), &finally(sub {
    $fin = 1;
}));
print "fin=$fin\n";

my $v = &try(sub { 42 }, &catch(sub { 0 }));
print "val=$v\n";

my $u = &try(sub { die "x" }, &catch(sub { "handled" }));
print "handled=$u\n";

&try(sub {
    1;
}, &catch(sub {
    print "should_not\n";
}), &finally(sub {
    print "finally_ok\n";
}));

eval {
    &try(sub { die "uncaught" });
};
print "uncaught=", ($@ =~ /uncaught/ ? 1 : 0), "\n";

print "done\n";
