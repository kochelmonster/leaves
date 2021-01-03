#!/usr/bin/env python
"""
create a graphiz file from graph.yaml

usage: python tests/graph.py tests/cmpfiles/*
"""
import sys
import yaml
import sarge
from pathlib import Path


CNODE = """"{id}" [fillcolor=yellow label="{{{{{id}|size: {size}}}|{keys}}}"]"""
VNODE = """"{id}" [label="{{{{{id}|size: {size}}}|{value}}}"]"""
TNODE = """"{id}" [fillcolor=azure label="{{{{{id}|bits: {bits}}}|{{{slots}}}}}"]"""


class Graph:
    def make_graph(self, nodes):
        lines = ["digraph G {", "node [shape=record style=filled]"]
        add = lines.append

        for n in nodes:
            type_ = n["id"].split("-", 1)[0]
            add(getattr(self, "handle_" + type_)(n))

        for n in nodes:
            type_ = n["id"].split("-", 1)[0]
            if type_ != "kTrie":
                for c in n.get("children", ()):
                    if c != "kNull-0-0":
                        add(f'"{n["id"]}" -> "{c}"')
            else:
                for i, c in enumerate(n.get("children", ())):
                    add(f'"{n["id"]}":f{i} -> "{c}"')

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

    def handle_kNull(self, node):
        return "\"{id}\"".format(**node)


def main(paths):
    for p in paths:
        src = Path(p)
        dest = Path("graphs")/(src.stem + ".svg")
        print("dest", dest)

        with open(src, "r") as f:
            nodes = yaml.load_all(f.read(), Loader=yaml.FullLoader)
            sarge.run(f"dot -Tsvg > {dest}", input=Graph().make_graph(list(filter(bool, nodes))))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
