#!/usr/bin/perl
# D223: goto past `my` must not leave the scope-exit free undominated,
# and the skipped lexical stays undef.
use strict;
use warnings;
sub skipped {
    my $outer = "keep";
    if (1) {
        my $encoding = "init";
        goto set_1252 if 1;
        my $copy = "nope";
        print "fell\n";
        set_1252:
        $encoding = "CP1252";
        my $seen = defined($copy) ? $copy : "undef";
        print "$encoding $seen\n";
    }
    print "$outer\n";
}
skipped();
