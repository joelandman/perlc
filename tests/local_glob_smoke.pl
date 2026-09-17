$_ = "orig";
sub show { return "[$_]"; }
sub setglob { local *_ = \join('', "x", "y"); return show(); }
print setglob(), "\n";
print "after: [$_]\n";
print "smoke_done\n";
