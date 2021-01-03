"""
compare graph files on equality if the node names have changed

usage: python compare.py dir1|file1 dir2|file2
"""
import sys
import yaml
from pathlib import Path


def transform(nodes):
    id_ = 0
    nmap = {}

    for n in nodes:
        if n is None:
            continue
        name = n["id"]
        type_ = name.split("-", 1)[0]
        new_name = f"{type_}-{id_}"
        id_ += 1
        nmap[name] = n["id"] = new_name

    for n in nodes:
        if n is None:
            continue
        try:
            n["children"] = [nmap.get(c, c) for c in n["children"]]
        except KeyError:
            pass

    return nodes


def compare(nodes1, nodes2):
    nodes1 = transform(nodes1)
    nodes2 = transform(nodes2)
    """
    if nodes1 != nodes2:
        import pprint
        pprint.pprint(nodes1)
        print("----------")
        pprint.pprint(nodes2)
    """
    return nodes1 == nodes2


def load(path):
    with open(path, "r") as f:
        return list(yaml.load_all(f.read(), Loader=yaml.FullLoader))


def main(cmp1, cmp2):
    cmp1 = Path(cmp1)
    cmp2 = Path(cmp2)

    if cmp1.is_dir:
        files1 = {f.name: f for f in cmp1.glob("*.yaml")}
        files2 = {f.name: f for f in cmp2.glob("*.yaml")}
    else:
        files1 = {cmp1.name: cmp1}
        files2 = {cmp2.name: cmp2}

    for n, f1 in files1.items():
        try:
            f2 = files2[n]
        except KeyError:
            print(f"{n} does not exist in {cmp2}")
            continue

        # print("compare", n)
        if compare(load(f1), load(f2)):
            print("equal:", n)
        else:
            print("---------------------")
            print("!!!not equal", n)
            print("---------------------")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
