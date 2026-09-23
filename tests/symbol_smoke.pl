use Symbol;
print "q1=", qualify("foo"), "\n";
print "q2=", qualify("Bar::baz"), "\n";
print "q3=", qualify("x", "Pkg"), "\n";
my $g = gensym();
print "gs_defined=", (defined $g ? 1 : 0), "\n";
print "done\n";
