use lib 'tests/lib';
use strict;
use warnings;

# MVP #4 — caller-namespace injection regression.
# `import()` must be scoped to the CALLER's package. When `use` runs inside
# `package Sub`, the exported symbol is available only in Sub, not leaked
# into main. perlc previously used a single global importMap, so the bareword
# resolved in every package (leaked into main).
{
    package Sub;
    use E::ScopedOne;
    print "in Sub: ", scoped_one(), "\n";
}
print "defined in main: ", (defined(&scoped_one) ? "yes" : "no"), "\n";
print "done\n";
