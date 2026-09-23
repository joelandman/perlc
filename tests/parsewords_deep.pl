use Text::ParseWords;
print "sw=", join("|", shellwords("a b \"c d\"")), "\n";
print "keep=", join("|", quotewords(" ", 1, "a \"b c\" d")), "\n";
print "nokeep=", join("|", quotewords(" ", 0, "a \"b c\" d")), "\n";
print "comma=", join("|", parse_line(",", 0, "x,y,z")), "\n";
print "done\n";
