#!/usr/bin/env python
# python tests/graph.py tests/cmpfiles/*
import sys
import yaml
import sarge
from pathlib import Path


CNODE = """"{id}" [fillcolor=yellow label="{{{{{id}|size:{size}/{space}}}|{keys}}}"]"""
LNODE = """"{id}" [fillcolor=darkolivegreen1 label="{{{{{id}}}|size: {size}/{space}|{value}}}"]"""
MLNODE = """"{id}" [fillcolor=darksalmon label="{{{{{id}|space: {space}}}}}}}"]"""
LINODE = """"{id}" [label="{{{id}|space: {space}}}}}"]"""
TNODE = """"{id}" [fillcolor=azure label="{{{{{id}|space: {space}|bits: {bits}}}|{{{slots}}}}}"]"""


class Graph:
    def make_graph(self, nodes):
        self.upper_bits = {}
        self.nodes = {}

        lines = ["digraph G {", "layout=dot", "node [shape=record style=filled]"]
        add = lines.append

        # group nodes
        pages = {}
        for n in nodes:
            self.nodes[n["id"]] = n
            page = n["id"].rsplit("P", 1)[1]
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

    def handle_kCompressed(self, node):
        return CNODE.format(**node)

    def handle_kValue(self, node):
        if len(node["value"]) > 9:
            node["value"] = node["value"][:9] + "..."
        return LNODE.format(**node)
    
    def handle_kHeapLink(self, node):
        return MLNODE.format(**node)
    
    def handle_kLink(self, node):
        return LINODE.format(**node)

    def handle_kUpperTrie(self, node):
        for i, c in enumerate(node["children"]):
            id_ = c
            if id_.startswith("kLink"):
                id_ = self.nodes[id_]["children"][0]
            self.upper_bits[id_] = node["bitindex"][i]
        
        return self.handle_kTrie(node)

    def handle_kLowerTrie(self, node):
        upper_bit = self.upper_bits[node["id"]]
        chars = [chr((upper_bit << 4) + lb)+"("+str(lb)+")" for lb in node["bitindex"]]
        return self.handle_kTrie(node, chars)

    def handle_kTrie(self, node, chars=None):
        byteindex = node.get("byteindex")
        if chars:
            slots = "|".join(f"<f{i}> {b}" for i, b in enumerate(chars))
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
            # print(Graph().make_graph(list(filter(bool, nodes))))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

