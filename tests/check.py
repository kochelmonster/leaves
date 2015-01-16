#!/usr/bin/env python
import sys
import yaml


class Node(object):
    def __init__(self, page, id_, kind):
        self.page = page
        self.id = id_
        self.parent = None
        self.kind = kind

    def check(self):
        if not self.parent:
            if self.page == 0 and self.id == 0:
                return #root
            print "**error node not connected", self.id, self.page, self.kind


class GraphChecker(object):
    def __init__(self):
        self.nodes = {}
        self.connections = []
    
    def __call__(self, state, no):
        for page in state["state"]:
            for node in page["nodes"]:
                method = getattr(self, "analyze_"+node["type"])
                method(page, node)

        for src, dst in self.connections:
            self.nodes[dst].parent = src
            
        for n in self.nodes.values():
            n.check()
                
    def _analyze_trie_kind(self, page, node, kind):
        page_id = int(page["id"])
        node_id = int(node["id"])
        if node["data"]:
            connections = (int(p.split(">")[1]) for p in node["data"].split("|"))
            self.connections.extend(
                ((page_id, node_id), (page_id, c)) for c in connections)
        self.nodes[page_id, node_id] = Node(page_id, node_id, kind)

    def analyze_bittrie(self, page, node):
        self._analyze_trie_kind(page, node, "bittrie")
    
    def analyze_trie(self, page, node):
        self._analyze_trie_kind(page, node, "trie")

    def analyze_compressed(self, page, node):
        page_id = int(page["id"])
        node_id = int(node["id"])
        self.connections.append(
                ((page_id, node_id), (page_id, int(node["child"]))))
        self.nodes[page_id, node_id] = Node(page_id, node_id, "compressed")

    def analyze_leaf(self, page, node):
        page_id = int(page["id"])
        node_id = int(node["id"])
        self.nodes[page_id, node_id] = Node(page_id, node_id, "leaf")

    def analyze_link(self, page, node):
        page_id = int(page["id"])
        node_id = int(node["id"])
        self.connections.append(
                ((page_id, node_id), (int(node["page"]), 0)))
        self.nodes[page_id, node_id] = Node(page_id, node_id, "link")
    

def main(instream):
    all_data = []
    while True:
        data = instream.read()
        if not data:
            break
        all_data.append(data)

    obj = yaml.load_all("".join(all_data))
    i = 0
    for state in obj:
        if state is None:
            continue
        if "state" not in state:
            continue

        GraphChecker()(state, i)
        i += 1

if __name__ == "__main__":
    main(sys.stdin)
