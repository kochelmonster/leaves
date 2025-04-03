
BLOCK_SIZES = [
    32,    64,    96,    128,   160,   192,   224,   256,   288,   320,   352,
    384,   416,   448,   480,   512,   576,   640,   704,   768,   832,   896,
    960,   1024,  1152,  1280,  1408,  1536,  1664,  1792,  1920,  2048,  2304,
    2560,  2816,  3072,  3328,  3584,  3840,  4096,  4608,  5120,  5632,  6144,
    6656,  7168,  7680,  8192,  9216,  10240, 11264, 12288, 13312, 14336, 15360,
    16384, 18432, 20480, 22528, 24576, 26624, 28672, 30720, 32768, 36864, 40960,
    45056, 49152, 53248, 57344, 61440, 65536
]


def assign_next_block(size):
    for i, block_size in enumerate(BLOCK_SIZES):
        if size <= block_size:
            return i
    return -1


def highest_bit(size):
    return size.bit_length()-1


def fast_assign_block(size):
    size -= 1
    for i in range(9, 17):
        if (size >> i) == 0: return (size >> (i-4)) + (i-9)*8
    return -1


def static_assign_block(size):
    size -= 1
    bit = highest_bit(size)
    match bit:
        case 8: return size >> 5
        case 9: return 8 + size >> 6
        case 10: return 16 + size >> 7
        case 11: return 24 + size >> 8
        case 12: return 32 + size >> 9
        case 13: return 36 + size >> 10
        case 14: return 40 + size >> 11
        case 15: return 48 + size >> 12
        case 16: return 56 + size >> 13
        
    return size >> 5 if bit < 8 else -1


def block_assign_test():
    for i in range(1, 64*1024):
        if not fast_assign_block(i) == assign_next_block(i) == static_assign_block(i):
            print("wrong", i, fast_assign_block(i), assign_next_block(i), static_assign_block(i))
            print(highest_bit(i-1))
            break
    


def test_hbit(number):
    bit = highest_bit(number)
    print(number, ": ", bit, "|", 1<<bit, 1<<(bit+1))


def main():
    # block_assign_test()
    #for b in BLOCK_SIZES:
    #    test_hbit(b-1)

    block_assign_test()


if __name__ == "__main__":
    main()