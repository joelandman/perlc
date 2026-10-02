# Deep test for D181: require vVERSION (require v5.8.1;) was a hard
# "unexpected token '.'" parse error -- it lexes as IDENT "v5" DOT INT
# "8" DOT INT "1", and require's VERSION handling only recognized a
# plain FLOAT/INT form (require 5.008;). No-op like the numeric form
# (host is always new enough); fixed by recognizing a leading v-prefixed
# identifier and consuming any trailing .NUMBER components.
require v5.8.1;
print "after_v_dotted=ok\n";

require v5.36;
print "after_v_two_part=ok\n";

require v5;
print "after_v_bare=ok\n";

# regression: plain numeric VERSION form stays unaffected
require 5.008;
print "after_numeric=ok\n";

# regression: ordinary module-name require stays unaffected
require POSIX;
print "after_module=ok\n";

print "done\n";
