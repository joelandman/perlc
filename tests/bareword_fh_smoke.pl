# D152 smoke: close/eof/tell/binmode/fileno on a bareword filehandle that
# was never opened compiled as an unknown sub call ("String found where
# operator expected"); perl returns false/undef at runtime.
sub v { defined $_[0] ? "[$_[0]]" : "u" }
print "close ", v(close(NEVEROPENED)), "\n";
close NEVEROPENED2;
print "eof ", (eof(NOPE) ? "T" : "F"), " fileno ", v(fileno(NOPE)), "\n";
print "tell ", tell(NOPE), " binmode ", v(binmode(NOPE)), "\n";
