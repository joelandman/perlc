#!/usr/bin/perl
# Regex-heavy benchmark: grep { /pattern/ } @data in a tight loop
# Demonstrates the PCRE2 pattern cache benefit

my @data = ();
for my $i (1..100000) {
    push @data, "item_$i" . ($i % 3 == 0 ? "_match" : "");
}

my $count = 0;
for my $i (1..50) {
    for my $item (@data) {
        $count++ if $item =~ /item_\d+_match/;
    }
}

print "matches: $count\n";
