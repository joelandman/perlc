#!/usr/bin/perl
# Deep test for D116: __PACKAGE__/__FILE__/__LINE__ — previously a hard
# parse error ("String found where operator expected (Do you need to
# predeclare ...)"), despite `bless {...}, __PACKAGE__` being one of the
# most common OO-Perl idioms in CPAN modules.
use strict;
use warnings;

my @failures;
sub check {
    my ($name, $ok) = @_;
    print $name, "=", ($ok ? "ok" : "FAIL"), "\n";
    push @failures, $name unless $ok;
}

package Foo::Bar;
sub whoami { return __PACKAGE__; }
package main;
check('package_in_sub', Foo::Bar::whoami() eq "Foo::Bar");
check('package_at_top', __PACKAGE__ eq "main");

check('file_is_nonempty', length(__FILE__) > 0);
check('file_ends_right', __FILE__ =~ /d116_dunder_consts_deep\.pl$/);

my $line_a = __LINE__;
my $line_b = __LINE__;
check('line_increments', $line_b == $line_a + 1);

# __PACKAGE__->method() — the canonical OO constructor idiom
package Animal;
sub new {
    my $class = shift;
    return __PACKAGE__->create(@_);
}
sub create {
    my ($class, %args) = @_;
    return bless { %args }, $class;
}
package main;
my $a = Animal->new(name => "Rex");
check('package_arrow_constructor', ref($a) eq "Animal" && $a->{name} eq "Rex");

# fat-arrow context still auto-quotes __PACKAGE__ as a literal bareword
# string, matching real Perl — must not be resolved there
my %h = (__PACKAGE__ => "literal");
check('fatarrow_autoquote_unaffected', $h{__PACKAGE__} eq "literal");

if (@failures) {
    print "UNEXPECTED_FAILURES=", join(",", @failures), "\n";
    die "UNEXPECTED FAILURES: " . join(",", @failures) . "\n";
}
print "d116_dunder_consts_done\n";
