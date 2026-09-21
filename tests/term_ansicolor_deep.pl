use Term::ANSIColor qw(color colored RED GREEN BOLD RESET ON_BLUE);

print "red_empty=", (color("red") eq "" ? 1 : 0), "\n";
print "colored=", colored("hi", "green"), "\n";
print "consts=", join(",", map { $_ eq "" ? 1 : 0 } (RED, GREEN, BOLD, RESET, ON_BLUE)), "\n";
print "empty=", (color("not_a_color") eq "" ? 1 : 0), "\n";
print "done\n";
