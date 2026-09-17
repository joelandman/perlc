$_ = "orig";
sub show { return "[$_]"; }
sub setglob { local *_ = \join('', "x", "y"); return show(); }
print setglob(), "\n";
print "after: [$_]\n";
sub restore { local $_ = "tmp"; return show(); }
print restore(), "\n";
print "after2: [$_]\n";
sub outer_split { local *_ = \join('', "p q"); my @r = split(/ /, $_); return @r; }
my @w = outer_split();
print "split: @w\n";
print "after3: [$_]\n";
sub read_plain { return "p=[$_]"; }
print read_plain(), "\n";
my $saved = $_;
print "saved: $saved\n";
print "deep_done\n";
