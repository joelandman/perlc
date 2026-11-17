package E::UnlessFlag;
use strict;
use warnings;
use Exporter 'import';
# Regression fixture for MVP #4 (conditional imports). See E::IfFlag; this
# one exercises the `use unless` path (imports when the condition is FALSE).
our @EXPORT = qw(val);
sub val { return "UNLESS-OK"; }
1;
