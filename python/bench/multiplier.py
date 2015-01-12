"""
makes a really big string list
"""
import sys
from random import sample, randint

def read_words():
    with open("words.txt", "r") as f:
        return f.read().split()


def multiplier(count, word_count=10):
    words = read_words()
    with open("strings.txt", "w") as f:
        for i in xrange(count):
            sentence = sample(words, randint(1, word_count))
            f.write(" ".join(sentence))
            f.write("\n")
            if i % 1000 == 0:
                print "generated", i, "sentences"


if __name__ == "__main__":
    multiplier(long(sys.argv[1]))
            
