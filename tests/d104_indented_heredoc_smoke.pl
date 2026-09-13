#!/usr/bin/perl
# Smoke test for D104: `<<~IDENT` indented heredoc (Perl 5.26+) was a
# hard parse error.
use strict;
use warnings;

my $x = <<~END;
    hello
    END
print $x;
