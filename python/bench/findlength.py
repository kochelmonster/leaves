"""
finf the maximum length string in a string list
"""
import sys


def find_length(path):
    with open(path, "r") as f:
        max_len = 0
        i = 0
        l = f.readline()
        while l:
            max_len = max(len(l), max_len)
            if i % 1000 == 0:
                print "read", i, "sentences", max_len
            i += 1
            l = f.readline()

        print "max_length", max_len


if __name__ == "__main__":
    find_length(sys.argv[1])
            
