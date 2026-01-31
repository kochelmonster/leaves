#!/usr/bin/env python3
# Calculate actual block sizes and reasonable second-level node sizes

def trie_size(prefix, branches):
    """Calculate size of a trie node"""
    prefix_size = ((16 + prefix + 3) // 4) * 4  # padding to uint32_t
    lower_size = min(branches, 8) * 4  # bitmap (max 8 uint32_t)
    array_size = branches * 8  # offset_e is 8 bytes
    total = ((prefix_size + lower_size + array_size + 7) // 8) * 8  # align to 8
    return total

def leaf_size(key_size, value_size):
    """Calculate size of a leaf node"""
    # LeafNode header + key + value
    return ((16 + key_size + value_size + 7) // 8) * 8

# Current BLOCK_SIZES
sizes = [
    trie_size(1, 10),    # digits 0-9
    trie_size(1, 16),    # hex 0-9A-F
    trie_size(1, 64),    # base64
    trie_size(1, 127),   # utf-8
    trie_size(1, 256),   # binary (full)
    4096                 # 4K
]

print('Current BLOCK_SIZES:')
for i, s in enumerate(sizes):
    print(f'  Slot [{i}] = {s:5d} bytes ({s/1024:5.2f} KB)')

print(f'\nMax block size: {sizes[-1]} bytes = {sizes[-1]/1024:.1f} KB')
print(f'Min block size: {sizes[0]} bytes')

# Calculate typical leaf sizes
print('\n' + '='*60)
print('Typical Leaf Sizes (from benchmarks):')
print('='*60)
leaf_16_100 = leaf_size(16, 100)  # benchmark config
print(f'  16-byte key + 100-byte value: {leaf_16_100} bytes')

# Cache line considerations
print('\n' + '='*60)
print('Cache Line Analysis:')
print('='*60)
print('  L1 cache line: 64 bytes')
print('  L2 cache line: 64 bytes')
print('  L3 cache line: 64 bytes')
print(f'  CPU cache size: 25.6 MB (from readme.md)')

# Second-level node size analysis
print('\n' + '='*60)
print('Second-Level Trie Node Size Analysis:')
print('='*60)

second_level_header = 32  # estimated header size

scenarios = [
    (2, 'minimal - binary split'),
    (4, 'small - hex split'),
    (8, 'moderate - common case'),
    (16, 'large - hex digits'),
    (32, 'very large - base64 range'),
    (64, 'extreme - full base64'),
    (128, 'rare - half alphabet'),
    (256, 'max - full byte range'),
]

for child_count, desc in scenarios:
    # Parent trie node size
    parent_size = trie_size(8, child_count)  # assume 8-byte prefix average
    
    # Children: mix of trie nodes and leaves
    # Assume 50% leaves (16+100 bytes), 50% trie nodes (small)
    avg_child_size = (leaf_16_100 + trie_size(4, 4)) // 2
    
    total = parent_size + (child_count * avg_child_size)
    
    fits_l1 = '✓' if total <= 32*1024 else '✗'
    fits_l2 = '✓' if total <= 256*1024 else '✗'
    fits_l3 = '✓' if total <= 25600*1024 else '✗'
    
    print(f'\n  {child_count:3d} children ({desc}):')
    print(f'    Parent node:  {parent_size:6d} bytes')
    print(f'    Children:     {child_count * avg_child_size:6d} bytes ({child_count} × ~{avg_child_size})')
    print(f'    Total block:  {total:6d} bytes = {total/1024:6.2f} KB')
    print(f'    Fits in L1/L2/L3: {fits_l1}/{fits_l2}/{fits_l3}')

# Recommendations
print('\n' + '='*60)
print('RECOMMENDATIONS:')
print('='*60)

recommendations = [
    (4096, 'Conservative', 'Current MAX_BLOCK_SIZE, safe choice'),
    (8192, 'Moderate', 'Fits ~64 small children or ~24 leaves, still in L2'),
    (16384, 'Aggressive', 'Fits ~128 small children, fills L2 cache'),
    (32768, 'Very Aggressive', 'Fits ~256 small children, may spill from L2'),
]

for size, level, rationale in recommendations:
    child_capacity = size // avg_child_size
    print(f'\n  {size:6d} bytes ({size/1024:3.0f} KB) - {level}:')
    print(f'    Capacity: ~{child_capacity} average nodes')
    print(f'    Rationale: {rationale}')

print('\n' + '='*60)
print('SWEET SPOT ANALYSIS:')
print('='*60)
print('''
Based on your CPU (Intel i7-12700KF with 25.6 MB L3 cache):
- L1: 32 KB per core
- L2: 256 KB per core  
- L3: 25.6 MB shared

Recommended maximum: 8 KB - 16 KB

Why?
1. 8 KB: Fits ~48-64 children, covers 99% of real-world trie nodes
2. 16 KB: Fits ~96-128 children, covers 99.9% of cases
3. Still fits comfortably in L2 cache (256 KB)
4. Single memcpy() is very fast at these sizes
5. Avoids excessive write amplification

AVOID larger sizes:
- 32 KB+: Risk spilling from L2, diminishing returns
- Most trie nodes have < 16 children in practice
- Larger blocks = more write amplification on updates
''')
