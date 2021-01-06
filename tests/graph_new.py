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
LNODE = (""""{id}" [fillcolor=blanchedalmond """
         """label="{{{{{id}|size: {size}}}|{chars}|{{{slots}}}}}"]""")


LNODE = """{id} [shape=box, fillcolor=blanchedalmond, label=<
    <TABLE>
        <TR><TD><TABLE><TR><TD>{id}</TD><TD>{size}</TD></TR></TABLE></TD></TR>
        <TR><TD>{chars}</TD></TR>
        <TR><TD><TABLE><TR>{slots}</TR></TABLE></TD></TR>
    </TABLE>>]"""


class Graph:
    def make_graph(self, nodes):
        lines = ["digraph G {", "node [shape=record style=filled]", 'newrank=true']
        add = lines.append

        leaves = []

        for n in nodes:
            type_ = n["id"].split("-", 1)[0]
            line = getattr(self, "handle_" + type_)(n)
            if type_ == "kLeaf":
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
                for i, c in enumerate(n.get("children", ())):
                    if not c.startswith("kNull"):
                        add(f'"{n["id"]}":f{i} -> "{c}"')
            else:
                for c in n.get("children", ()):
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
            slots = '<TD PORT="f0">prev</TD><TD PORT="f1">trie</TD><TD PORT="f2">next</TD>'
        else:
            slots = '<TD PORT="f0">prev</TD><TD PORT="f1">next</TD>'

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
            print(Graph().make_graph(list(filter(bool, nodes))))
            sarge.run(f"dot -Tsvg > {dest}", input=Graph().make_graph(list(filter(bool, nodes))))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
