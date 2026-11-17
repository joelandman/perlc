package E::IfFlag;
use strict;
use warnings;
use Exporter 'import';
# Regression fixture for MVP #4 (conditional imports).
# A quoted `use if` / `use unless` with a ::-containing module name must
# resolve the module and honor the condition. When the condition fires,
# this module's @EXPORT is imported; when it does not fire, it is absent.
our @EXPORT = qw(enabled);
sub enabled { return "FLAG-ON"; }
1;
