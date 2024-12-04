#!/usr/bin/env python
# python tests/graph.py tests/cmpfiles/*
import sys
import yaml
import sarge
from pathlib import Path

MAX_VAL_SIZE = 27


CNODE = """"{id}" [fillcolor=yellow label="{{{{{id}|size:{size}}}|{keys}}}"]"""
LNODE = (
    """"{id}" [fillcolor=darkolivegreen1 label="{{{{{id}}}|size: {size}|{value}}}"]"""
)
MLNODE = """"{id}" [fillcolor=darksalmon label="{{{{{id}}}}}}}"]"""
LINODE = """"{id}" [label="{{{id}}}}}"]"""
BTNODE = """"{id}" [fillcolor=azure label="{{{{{id}|bits: {bits}}}|{{{slots}}}}}"]"""
TNODE = """"{id}" [fillcolor=pink label="{{{{{id}}}|{{{slots}}}}}"]"""


class Graph:
    def make_graph(self, nodes):
        self.nodes = {}

        lines = ["digraph G {", "layout=dot", "node [shape=record style=filled]"]
        add = lines.append

        # group nodes
        pages = {}
        for n in nodes:
            self.nodes[n["id"]] = n
            page = n["id"].rsplit("-", 1)[1]
            n["type"] = n["id"].split("-", 1)[0]
            pages.setdefault(page, []).append(n)

        for k, pnodes in pages.items():
            try:
                size = pnodes[0]["pspace"]
            except KeyError:
                print("wrong node", pnodes[0])
                raise
            add(f"subgraph cluster_Page{k} {{")
            add(f'label = "{{{k}|size: {size}}}"')
            # add nodes
            for n in pnodes:
                type_ = n["type"]
                add(getattr(self, "handle_" + type_)(n))
            add("}")

        # add connections
        for n in nodes:
            type_ = n["id"].split("-", 1)[0]
            if not type_.endswith("Trie"):
                for c in n.get("children", ()):
                    if c != "kNull-0P0":
                        add(f'"{n["id"]}" -> "{c}"')
            else:
                for i, c in enumerate(n.get("children", ())):
                    add(f'"{n["id"]}":f{i} -> "{c}"')

        add("}")
        add("")

        print("max page", max(map(int, pages.keys())))
        return "\n".join(lines)

    def handle_kString(self, node):
        return CNODE.format(**node)

    def handle_kValue(self, node):
        if len(node["value"]) > MAX_VAL_SIZE:
            node["value"] = node["value"][:MAX_VAL_SIZE] + "..."
        return LNODE.format(**node)

    def handle_kLink(self, node):
        return LINODE.format(**node)

    def handle_kUpperTrie(self, node):
        return self.trie(node)

    def handle_kLowerTrie(self, node):
        return self.trie(node)

    def handle_kArray(self, node):
        return self.trie(node)

    def trie(self, node):
        chars = node.get("bytes", "")
        slots = "|".join(f"<f{i}> {b}" for i, b in enumerate(chars))
        return TNODE.format(slots=slots, **node)

    def handle_kNull(self, node):
        return '"{id}"'.format(**node)


def main(paths):
    for p in paths:
        src = Path(p)
        dest = Path(__file__).parent / "graphs" / (src.stem + ".svg")
        print("dest", src, dest)

        with open(src, "r") as f:
            nodes = yaml.load_all(f.read(), Loader=yaml.FullLoader)
            #print(Graph().make_graph(list(filter(bool, nodes))))
            #return 0
            sarge.run(
                f"dot -Tsvg > {dest}",
                input=Graph().make_graph(list(filter(bool, nodes))),
            )

    return 0


if __name__ == "__main__":
    debug = [
        "cmpfiles/insert_trie_lower_grow_7_aG.yaml"
    ]

    debug = [Path(__file__).parent / d for d in debug]
    sys.exit(main(sys.argv[1:] or debug))
