use lib 'tests/lib';

# Bare use: the exported one() resolves to the user sub (returns 42), not a
# builtin that demands a list argument.
use E::NamedOne;
printf "bareuse:one=%d\n", eval { one() } // -1;

# Explicit list form.
use E::NamedOne qw(one);
printf "explicit:one=%d\n", eval { one() } // -1;

# Qualified call still reaches the user sub directly.
printf "qualified:one=%d\n", eval { E::NamedOne::one() } // -1;

print "deep_done\n";
