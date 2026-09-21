use Try::Tiny;
&try(sub { die "boom" }, &catch(sub { print "caught\n" }));
&try(sub { print "ok\n" }, &catch(sub { print "no\n" }));
print "done\n";
