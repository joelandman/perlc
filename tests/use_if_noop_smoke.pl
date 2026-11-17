use lib 'tests/lib';
use strict;
use warnings;

# MVP #4 — the NO-OP branch of conditional `use if` / `use unless`.
# A false condition must leave the module un-imported, so the exported
# barewords stay absent. This guards against a condition-logic inversion
# (importing when the condition is false) that the positive-path test above
# would not catch on its own.
use if 0,  "E::IfFlag";
use unless 1, "E::UnlessFlag";

print "if_enabled=",  (defined(&enabled)  ? enabled()  : "absent"), "\n";
print "unless_val=",  (defined(&val)      ? val()      : "absent"), "\n";
print "done\n";
