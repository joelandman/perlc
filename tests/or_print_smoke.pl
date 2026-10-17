# D194: high-precedence || print FILEHANDLE (splain).
sub drop { 0 }
sub keep { 1 }
*THITHER = 1 ? *STDOUT : *STDERR;
drop() || print THITHER "via\n";
keep() || print THITHER "nope\n";
