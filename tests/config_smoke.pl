use Config;
print $Config{archname}, "\n";
print $Config{version}, "\n";
print exists $Config{cc} ? 1 : 0, "\n";
print exists $Config{no_such_key_xyz} ? 1 : 0, "\n";
print "smoke_done\n";
