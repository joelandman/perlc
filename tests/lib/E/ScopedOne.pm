package E::ScopedOne;
use strict;
use warnings;
use Exporter 'import';

# Regression fixture for MVP #4 (caller-namespace injection): a module
# whose imported symbol is used from BOTH the importing package and from
# main. Real Perl scopes `import()` to the caller's package, so a `use`
# inside `package Sub` makes the exported name available only in Sub, not
# leaked into main. perlc used a single global importMap, so the bareword
# resolved in every package and leaked.
our @EXPORT = qw(scoped_one);

sub scoped_one { return 7; }

1;
