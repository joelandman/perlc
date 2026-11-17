use lib 'tests/lib';
use E::DefaultTag qw(:DEFAULT);
print "foo=", (eval { foo() } ? foo() : "FAIL"), "\n";
print "bar=", (eval { bar() } ? bar() : "FAIL"), "\n";
# baz is @EXPORT_OK, NOT in @EXPORT, so :DEFAULT must NOT import it
print "baz=", (eval { baz() } ? baz() : "unim"), "\n";
print "smoke_done\n";
