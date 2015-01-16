"""
makes a really big string list
"""
import sys


def create_number(count):
    width = len(str(count-1))
    template = "{{:0{}}}".format(width)
    with open("numbers.txt", "w") as f:
        for i in xrange(count):
            f.write(template.format(i))
            f.write("\n")
            if i % 1000 == 0:
                print "generated", i, "numbers"


if __name__ == "__main__":
    create_number(long(sys.argv[1]))
            
