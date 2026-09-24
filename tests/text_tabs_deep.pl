use Text::Tabs;
print "[", expand("\thello"), "]\n";
print "[", expand("a\tb"), "]\n";
$Text::Tabs::tabstop = 4;
print "[", expand("a\tb"), "]\n";
print "done\n";
