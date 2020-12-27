#!/usr/bin/env python
import sys
import yaml
import sarge
from pathlib import Path


CNODE = """"{id}" [fillcolor=yellow label="{{{{{id}|size: {size}}}|{keys}}}"]"""
NNODE = """"{id}" [label="{id}|size: {size}"]"""
TNODE = """"{id}" [fillcolor=azure label="{id}|bits: {bits}"]"""


class Graph:
    def make_graph(self, nodes):
        lines = ["digraph G {", "node [shape=record style=filled]"]
        add = lines.append

        for n in nodes:
            type_ = n["id"].split("-", 1)[0]
            add(getattr(self, "handle_" + type_)(n))

        for n in nodes:
            for c in n.get("children", ()):
                if c != "kNull-0-0":
                    add(f'"{n["id"]}" -> "{c}"')

        add("}")
        add("")

        return "\n".join(lines)

    def handle_kCompressed(self, node):
        return CNODE.format(**node)

    def handle_kValue(self, node):
        return NNODE.format(**node)

    def handle_kTrie(self, node):
        return TNODE.format(**node)

    def handle_kNull(self, node):
        return "{id}".format(**node)


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
