package My::Pxs;
use strict; use warnings;

sub double_it { return $_[0] * 2; }
sub greet     { return "pxs-hello: $_[0]"; }

# boot hook, perlc convention: <Module>::boot (the loader also tries
# <Module>::boot_<mangled> and bare boot_<mangled>, real DynaLoader's
# C-symbol name shape).
sub boot { return 1; }
1;
