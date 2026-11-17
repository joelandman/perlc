use lib 'tests/lib';
use strict;
use warnings;

# MVP #4 — conditional `use if` / `use unless` with a QUOTED :: module name.
# Regression: a quoted module name in `use if`/`use unless` previously failed
# to resolve (the lexer keeps a leading \x01 marker and surrounding quotes on
# the STRING token), so the name never matched the import-list walk-back and
# the module's @EXPORT was never imported -> the bareword was undefined.
#
# `use if` imports when the condition is TRUE; `use unless` imports when the
# condition is FALSE. Both must import here.
use if 1,  "E::IfFlag";
use unless 0, "E::UnlessFlag";

print "if_enabled=",  enabled(),  "\n";
print "unless_val=",  val(),      "\n";
print "done\n";
