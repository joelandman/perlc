use Text::ParseWords;
print join("|", shellwords("a b \"c d\"")), "\n";
print "done\n";
