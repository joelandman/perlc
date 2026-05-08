package Animal;

sub new {
    my $class = shift @_;
    my $name  = shift @_;
    my $sound = shift @_;
    my $self = { name => $name, sound => $sound };
    return bless $self, $class;
}

sub speak {
    my $self = shift @_;
    print $self->{name}, " says ", $self->{sound}, "\n";
}

sub name {
    my $self = shift @_;
    return $self->{name};
}

sub describe {
    my $self = shift @_;
    my $n = $self->{name};
    my $s = $self->{sound};
    print "I am $n and I say $s\n";
}

package Counter;

sub new {
    my $class = shift @_;
    my $start = shift @_;
    my $self = { count => $start };
    return bless $self, $class;
}

sub increment {
    my $self = shift @_;
    $self->{count} = $self->{count} + 1;
    return $self;
}

sub value {
    my $self = shift @_;
    return $self->{count};
}

package main;

my $dog = Animal->new("Rex", "woof");
my $cat = Animal->new("Whiskers", "meow");

$dog->speak();
$cat->speak();

print ref($dog), "\n";
print ref($cat), "\n";

print $dog->name(), "\n";

$dog->describe();

my $c = Counter->new(0);
$c->increment();
$c->increment();
$c->increment();
print $c->value(), "\n";

print ref($c), "\n";
