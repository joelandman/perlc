use Term::ANSIColor qw(color colored RED RESET);
print "color_empty=", (color("red") eq "" ? 1 : 0), "\n";
print "colored=", colored("hi", "red"), "\n";
print "RED_empty=", (RED eq "" ? 1 : 0), "\n";
print "done\n";
