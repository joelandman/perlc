#!/usr/bin/env python3


def p(x):
    # Perl stringifies numbers with %.15g; emulate so output matches
    # (e.g. 10/3 -> 3.33333333333333, 2.5*4 -> 10 not 10.0)
    if isinstance(x, float):
        return "%.15g" % x
    return str(x)


a = 10
b = 3

print(p(a + b))
print(p(a - b))
print(p(a * b))
print(p(a / b))
print(p(a % b))

x = 1
x += 5
print(p(x))

s = "Hello"
s += " World"
print(s)

n = 2.5
print(p(n * 4))
