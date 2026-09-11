#!/usr/bin/perl
# Smoke test for D129: local($var) = EXPR; (parenthesized single-var
# list-form local) was a hard parse error. Found verbatim in real,
# unmodified Pod::Usage.pm (`local($_) = shift;`).
use strict;
use warnings;

sub foo {
    local($_) = shift;
    print "$_\n";
}
foo("hi");
