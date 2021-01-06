#!/usr/bin/env python
"""
create a graphiz file from graph.yaml

usage: python tests/graph.py tests/cmpfiles/*
"""
import sys
import yaml
import sarge
from pathlib import Path


CNODE = """"{id}" [fillcolor=yellow label="{{{{{id}|size: {size}}}|{chars}}}"]"""
VNODE = """"{id}" [label="{{{{{id}|size: {size}}}|{value}}}"]"""
TNODE = """"{id}" [fillcolor=azure label="{{{{{id}|bits: {bits}}}|{{{slots}}}}}"]"""

LNODE = """"{id}" [shape=plaintext style="" margin="0,0" label=
    <<TABLE BGCOLOR="blanchedalmond" BORDER="0" CELLBORDER="1" CELLSPACING="0" CELLPADDING="5">
        <TR><TD COLSPAN="3">{id}</TD><TD>{size}</TD></TR>
        <TR><TD COLSPAN="4">{chars}</TD></TR>
    </TABLE>>]"""
LNODE = "".join(LNODE.splitlines())


class Graph:
    def make_graph(self, nodes):
        lines = ["digraph G {", "node [shape=record style=filled]", 'newrank="true"']
        add = lines.append

        leaves = []

        for n in nodes:
            type_ = n["id"].split("-", 1)[0]
            line = getattr(self, "handle_" + type_)(n)
            if type_ == "kLeaf" and len(n["children"]) < 3:
                leaves.append(line)
            else:
                add(line)

        add("subgraph subs {")
        add('rank="same"')
        lines.extend(leaves)
        add("}")

        for n in nodes:
            type_ = n["id"].split("-", 1)[0]

            if type_ == "kTrie":
                for i, c in enumerate(n.get("children", ())):
                    add(f'"{n["id"]}":f{i} -> "{c}"')
            elif type_ == "kLeaf":
                for c in n.get("children", ()):
                    if not c.startswith("kNull"):
                        add(f'"{n["id"]}" -> "{c}" [weight=0]')
            else:
                for c in n.get("children", ()):
                    if not c.startswith("kNull"):
                        add(f'"{n["id"]}" -> "{c}"')

        add("}")
        add("")

        return "\n".join(lines)

    def handle_kCompressed(self, node):
        return CNODE.format(**node)

    def handle_kValue(self, node):
        return VNODE.format(**node)

    def handle_kTrie(self, node):
        byteindex = node.get("byteindex")
        if byteindex:
            slots = "|".join(f"<f{i}> {b}" for i, b in enumerate(byteindex))
        else:
            slots = "|".join(f"<f{i}> {b}" for i, b in enumerate(node["bitindex"]))

        return TNODE.format(slots=slots, **node)

    def handle_kLeaf(self, node):
        if len(node["children"]) == 3:
            slots = "<f0> prev|<f1> trie|<f2> next"
        else:
            slots = "<f0> prev|<f1> next"

        return LNODE.format(slots=slots, **node)

    def handle_kNull(self, node):
        return "\"{id}\"".format(**node)


def main(paths):
    for p in paths:
        src = Path(p)
        dest = Path("graphs")/(src.stem + ".svg")
        print("dest", dest)

        with open(src, "r") as f:
            nodes = yaml.load_all(f.read(), Loader=yaml.FullLoader)
            # print(Graph().make_graph(list(filter(bool, nodes))))
            sarge.run(f"dot -Tsvg > {dest}", input=Graph().make_graph(list(filter(bool, nodes))))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
