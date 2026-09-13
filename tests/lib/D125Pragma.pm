package D125Pragma;
use Exporter 'import';
our @EXPORT    = qw(simple_double);
our @EXPORT_OK = qw(triple);

# A module that itself uses pragmas at top level, and the main script
# will additionally use this module inside a sub (D125 nested module-use).
use strict;
use warnings;

sub triple { my ($x) = @_; return $x * 3; }

1;
