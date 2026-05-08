#!/usr/bin/perl
use strict;
use warnings;

package Animal;

sub new {
    my ($class, $name, $sound) = @_;
    return bless { name => $name, sound => $sound }, $class;
}

sub name  { my ($self) = @_; return $self->{name};  }
sub sound { my ($self) = @_; return $self->{sound}; }

sub speak {
    my ($self) = @_;
    return $self->name() . " says " . $self->sound();
}

sub describe {
    my ($self) = @_;
    return "I am an animal named " . $self->name();
}

package Dog;
use parent 'Animal';

sub new {
    my ($class, $name) = @_;
    my $self = $class->SUPER::new($name, "woof");
    $self->{tricks} = [];
    return $self;
}

sub learn_trick {
    my ($self, $trick) = @_;
    push @{$self->{tricks}}, $trick;
}

sub show_tricks {
    my ($self) = @_;
    return join(", ", @{$self->{tricks}});
}

sub describe {
    my ($self) = @_;
    return "I am a dog named " . $self->name();
}

package Cat;
use parent 'Animal';

sub new {
    my ($class, $name) = @_;
    return $class->SUPER::new($name, "meow");
}

package main;

my $dog = Dog->new("Rex");
my $cat = Cat->new("Whiskers");

print $dog->speak() . "\n";        # Rex says woof
print $cat->speak() . "\n";        # Whiskers says meow
print ref($dog) . "\n";            # Dog
print ref($cat) . "\n";            # Cat
print $dog->describe() . "\n";     # I am a dog named Rex
print $cat->describe() . "\n";     # I am an animal named Whiskers

$dog->learn_trick("sit");
$dog->learn_trick("shake");
$dog->learn_trick("roll over");
print $dog->show_tricks() . "\n";  # sit, shake, roll over

my $generic = Animal->new("Fido", "bark");
print $generic->speak() . "\n";    # Fido says bark
print ref($generic) . "\n";        # Animal
