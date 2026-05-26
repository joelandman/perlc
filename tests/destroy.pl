#!/usr/bin/perl
use strict;
use warnings;

# Classes defined at top level
my $destroy_log = "";
my $destroy_count = 0;
my $last_name = "";

package Animal;
sub new {
    my ($class, $name) = @_;
    return bless { name => $name }, $class;
}
sub DESTROY {
    my ($self) = @_;
    $destroy_log .= "d:" . $self->{name} . ";";
    $destroy_count++;
}

package Counter;
sub new { my ($c) = @_; return bless {}, $c; }
sub DESTROY { $destroy_count++; }

package NameCapture;
sub new { my ($c,$n) = @_; return bless {n=>$n}, $c; }
sub DESTROY { my ($s)=@_; $last_name = $s->{n}; }

package main;

# Test 1: DESTROY fires on scope exit
$destroy_count = 0;
{
    my $a = Animal->new("cat");
    my $b = Animal->new("dog");
}
print "Test 1: ", $destroy_count == 2 ? "ok" : "FAIL(count=$destroy_count)", "\n";

# Test 2: DESTROY fires on explicit undef assignment
$destroy_count = 0;
my $x = Animal->new("fox");
$x = undef;
print "Test 2: ", $destroy_count == 1 ? "ok" : "FAIL(count=$destroy_count)", "\n";

# Test 3: DESTROY fires on overwrite, not for new value
$destroy_log = "";
my $y = Animal->new("old");
$y = Animal->new("new");
# old should be destroyed, new should not be (yet)
print "Test 3: ", ($destroy_log eq "d:old;") ? "ok" : "FAIL(log=$destroy_log)", "\n";

# Test 4: DESTROY fires in every loop iteration
$destroy_count = 0;
for my $i (1..5) {
    my $obj = Counter->new();
}
print "Test 4: ", $destroy_count == 5 ? "ok" : "FAIL(count=$destroy_count)", "\n";

# Test 5: DESTROY can access object data
$last_name = "";
{
    my $obj = NameCapture->new("hello");
}
print "Test 5: ", $last_name eq "hello" ? "ok" : "FAIL(got='$last_name')", "\n";
