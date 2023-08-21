import random


CHARS = list(range(256))


def calc_44(inserts):
    lower_tries = {}

    for c in inserts:
        ub = c >> 4
        lb = c & 0xF

        trie = lower_tries.get(ub, 0)
        if not trie & (1 << lb):
            trie |= (1 << lb)
            lower_tries[ub] = trie

    for k, v in lower_tries.items():
        print(k, ": ", v.bit_count(), v, 2+v.bit_count()*2)


    upper_size = 2 + len(lower_tries)*2 
    lower_size = sum((2 + v.bit_count()*2) for v in lower_tries.values())
    print(upper_size, lower_size)
    print(upper_size + lower_size)


def calc_62(inserts):
    lower_tries = {}

    for c in inserts:
        ub = c >> 2
        lb = c & 0x3

        trie = lower_tries.get(ub, 0)
        if not trie & (1 << lb):
            trie |= (1 << lb)
            lower_tries[ub] = trie

    for k, v in lower_tries.items():
        print(k, ": ", v.bit_count(), v, 2+v.bit_count()*2)

    upper_size = 4 + len(lower_tries)*2 
    lower_size = sum((1 + v.bit_count()*2) for v in lower_tries.values())
    print(upper_size, lower_size)
    print(upper_size + lower_size)

    
def show_sizes():
    inserts = random.sample(CHARS, 100)
    calc_44(inserts)
    print("----")
    calc_62(inserts)

    return
    for i in range(2, 200):
        inserts = random.sample(CHARS, i)
        print("chars", i, "4/4:", calc_44(inserts), "6/2", calc_62(inserts))



if __name__ == "__main__":
    show_sizes()