use FindBin;
print "script=$FindBin::Script\n";
print "bin_is_tests=", ($FindBin::Bin =~ m{/tests$} ? 1 : 0), "\n";
print "dir_eq=", ($FindBin::Dir eq $FindBin::Bin ? 1 : 0), "\n";
print "done\n";
