use Text::Wrap;
system("true");
# overflow chunking (default huge)
$Text::Wrap::columns = 10;
print "A=[" . fill("", "", "aaaaaaaaaaaaaaaaaaaaaaaaaa bb cc") . "]\n";
print "B=[" . wrap(">>", "--", "aaaaaaaaaaaaaaaaaaaaaaaaaa bb cc") . "]\n";
# huge=wrap — same chunking here
$Text::Wrap::huge = "wrap";
print "C=[" . fill("", "", "aaaaaaaaaaaaaaaaaaaaaaaaaa bb cc") . "]\n";
# exact-fit + word-boundary wraps
$Text::Wrap::columns = 20;
$Text::Wrap::huge = "overflow";
print "D=[" . fill("    ", "    ", "one two three four five six seven eight nine ten") . "]\n";
# separator2 wins over separator when set
$Text::Wrap::separator = "SEP";
$Text::Wrap::separator2 = "S2";
print "E=[" . fill("", "", "one two three four five six seven") . "]\n";
$Text::Wrap::separator2 = "";
$Text::Wrap::separator = "\n";
# fill paragraph splitting on \n\s+
print "F=[" . fill("", "", "para one words here\n     para two words also") . "]\n";
# tabs expand at 8
print "G=[" . wrap("", "", "ab\tcd") . "]\n";
# trailing whitespace preserved as the tail remainder
print "H=[" . wrap("", "", "aa bb   ") . "]\n";
print "done\n";
