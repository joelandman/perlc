#!/usr/bin/perl
use strict;
use warnings;

my $s = qq{2024-03-15 date=(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})};

if ($s =~ /(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})/) {
  say $+{year};    # 2024
  say $+{month};   # 03
  say $+{day};     # 15
  say join(' ', sort keys %+) eq 'day month year';  # 1
}

# /g
my $text = qq{cat (?<color>black) dog (?<color>white)};
my %last;
while ($text =~ /(?<animal>\\w+) \\((?<color>\\w+)\\)/g) {
  $last{animal} = $+{animal};
  $last{color} = $+{color};
}
say "$last{animal} is $last{color}";  # dog is white

# no match clears
$s !~ /nope/;
say keys %+ == 0;  # 1 (empty)

1;