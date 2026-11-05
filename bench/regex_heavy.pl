#!/usr/bin/perl
# Regex-heavy benchmark: grep { /pattern/ } @data in a tight loop
# Demonstrates the PCRE2 pattern cache benefit

# Sizes: (N, ROUNDS) — N elements, ROUNDS passes. Default keeps the quick
# shape the regression guard baseline was recorded at; the recorded
# results.csv row (bench.sh) passes its own size for a ~60s perl run.
my $n_data = defined $ARGV[0] ? $ARGV[0] : 100000;
my $rounds = defined $ARGV[1] ? $ARGV[1] : 50;

my @data = ();
for my $i (1..$n_data) {
    push @data, "item_$i" . ($i % 3 == 0 ? "_match" : "");
}

my $count = 0;
for my $i (1..$rounds) {
    for my $item (@data) {
        $count++ if $item =~ /item_\d+_match/;
    }
}

print "matches: $count\n";
