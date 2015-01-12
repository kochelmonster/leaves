#@+leo-ver=5-thin
#@+node:michael.20150110130802.32: * @file test_trie.py
#@@language python
#@@tabwidth -4
import unittest 
from larch.leaves import MemoryDatabaseBytes 


TEST_DATA = """
Tyler Serina Kuame Clio Lester Addison Dominique Eaton Keiko Lee Chester Dara
Vaughan Roth Astra Coby Bryar Octavius Clementine Astra Bradley Norman Joseph
Jameson Vaughan Clementine Derek Lavinia Jacqueline Cheyenne Jameson Oprah
Stella Fallon Cora Barrett Joel Hope Hyacinth Laura Conan Jerome Ezra Aline
Margaret Cameron Michael Jasmine Noelle Tamekah Cameron Mara Jared Trevor
Marshall May Lesley Daryl Aileen Donna Darrel Hedy Zena Gail Nola Colt Gay Clark
Felicia Selma Zahir Lavinia Veda Vielka Keegan Yoshio Inga Amal Yuli Ruth Nathan
Hall Cassidy Lacy Janna Galena Hadassah Charde Lucius Lester Elvis Henry Grant
Riley Acton Rose Galena Jescie Glenna Raymond
"""

TEST_DATA = TEST_DATA.split()


class MemotryDBTest(unittest.TestCase):
    def test_db(self):
        db = MemoryDatabaseBytes()
        cdict = {}
        for i, n in enumerate(TEST_DATA):
            db[n] = cdict[n] = i
            
        self.assertEqual(len(db), len(cdict))
            
        for k, v in cdict.items():
            self.assertEqual(db[k], v)
            
        for k, v in db:
            self.assertEqual(cdict[k], v)
            
        for i in range(10):
            del db[TEST_DATA[i]]
            del cdict[TEST_DATA[i]]
            
        self.assertEqual(len(db), len(cdict))
            
        for k, v in cdict.items():
            self.assertEqual(db[k], v)
            
        for k, v in db:
            self.assertEqual(cdict[k], v)            
            
            
if __name__ == "__main__":
    unittest.main()
            
#@-leo
