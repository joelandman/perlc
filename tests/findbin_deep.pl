use FindBin;
print "script=$FindBin::Script\n";
print "bin_is_tests=", ($FindBin::Bin =~ m{/tests$} ? 1 : 0), "\n";
print "dir_eq=", ($FindBin::Dir eq $FindBin::Bin ? 1 : 0), "\n";
print "realbin_eq=", ($FindBin::RealBin eq $FindBin::Bin ? 1 : 0), "\n";
print "realscript_suffix=",
      (substr($FindBin::RealScript, -length($FindBin::Script)) eq $FindBin::Script ? 1 : 0), "\n";
print "bin_isdir=", (-d $FindBin::Bin ? 1 : 0), "\n";
print "script_file=", (-f "$FindBin::Bin/$FindBin::Script" ? 1 : 0), "\n";
print "done\n";
