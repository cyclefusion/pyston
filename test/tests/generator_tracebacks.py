# expected: fail
# - wip

# Generators participate in the notional Python stack just like normal function calls do,
# even if we implement them using separate C stacks.
#
# This matters both for tracebacks, and some related things like sys.exc_info which should
# get inherited when we go into a generator

# exc_info gets passed into generators (at both begin and send()) and cleared like normal on the way out:
def f12():
    print
    print "f12"

    print "begin:", sys.exc_info()[0]

    def g():
        print "start of generator:", sys.exc_info()[0]
        yield 1
        print "after first yield:", sys.exc_info()[0]
        try:
            raise KeyError()
        except:
            pass
        print "after KeyError:", sys.exc_info()[0]
        yield 2

    try:
        raise AttributeError()
    except:
        pass

    i = g()
    i.next()
    try:
        1/0
    except:
        print "after exc:", sys.exc_info()[0]
        i.next()
        print "after next:", sys.exc_info()[0]
    list(i)
f12()

