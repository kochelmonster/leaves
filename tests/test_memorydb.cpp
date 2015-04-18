//#define BOOST_TEST_NO_MAIN
//#define GENERATE
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#define BOOST_TEST_MODULE TrieTest
#include <boost/test/included/unit_test.hpp>
#include "larch/leaves.h"
#include "node.h"
using namespace larch_leaves;

namespace larch_leaves {

void testpoint(const char* str) {
}

}

#ifdef GENERATE

std::ostream& out(std::cerr);

#undef BOOST_REQUIRE
#define BOOST_REQUIRE(x) assert(x)
 
#else

std::stringstream dumy;
std::ostream& out(dumy);
  
#endif
template<typename content_t> std::string 
value(content_t content, size_t size=0) {
  std::stringstream f;
  f << "value-" << content;
  std::string result(f.str());
  while(result.size() < size)
    result.push_back('-');
    
  return result;
}
struct TestMemoryDB {
  MemoryDatabase *db;
  
  std::string number(int number, size_t size=0) {
      std::stringstream f;
      f << std::setw(size) << std::setfill('0') << number;
      return f.str();
    }
    
  void test_Access()  {
      int i;
      std::unique_ptr<MemoryDatabase> db(MemoryDatabase::create());
      
      for(i = 0; i < 10000; i+=2) {
        std::string n = number(i, 6);
        db->find(n);
        db->set_value(n);
      }
      out << "generated! " << db->count() << std::endl;
      BOOST_REQUIRE(db->count() == 5000);

      for(db->first(), i = 0; db->is_valid(); db->next(), i+=2) {
        std::string n = number(i, 6);
        BOOST_REQUIRE(db->key() == n);
        BOOST_REQUIRE(db->value() == n);
      }
      
      BOOST_REQUIRE(i == 10000);
      out << "forward iteration passed" << std::endl;
      
      for(db->last(), i = 10000-2; db->is_valid(); db->prev(), i-=2) {
        std::string n = number(i, 6);
        BOOST_REQUIRE(db->key() == n);
        BOOST_REQUIRE(db->value() == n);
      }
      
      BOOST_REQUIRE(i == -2);
      out << "backward iteration passed" << std::endl;
            
      std::string n = number(1000, 6);
      db->find(n);
      BOOST_REQUIRE(db->is_valid());
      BOOST_REQUIRE(db->key() == n);
      BOOST_REQUIRE(db->value() == n);
      
      n = number(1001, 6);
      db->find(n);
      BOOST_REQUIRE(!db->is_valid());
      db->next();
      n = number(1002, 6);
      BOOST_REQUIRE(db->is_valid());
      BOOST_REQUIRE(db->key() == n);
      BOOST_REQUIRE(db->value() == n);
     
      n = number(1001, 6);
      db->find(n);
      BOOST_REQUIRE(!db->is_valid());
      db->prev();
      n = number(1000, 6);
      BOOST_REQUIRE(db->is_valid());
      BOOST_REQUIRE(db->key() == n);
      BOOST_REQUIRE(db->value() == n);
      out << "find passed" << std::endl;
      
      for(db->first(); db->is_valid(); db->next())
        db->remove();
      
      out << "removed passed" << db->count() << std::endl;
      for(db->first(); db->is_valid(); db->next())
        BOOST_REQUIRE(0);
        
      out << "removed passed" << db->count() << std::endl;
      BOOST_REQUIRE(db->count() == 0);
    }
    
    void test_Random() {
        std::unique_ptr<MemoryDatabase> db(MemoryDatabase::create());
                
        const char* data[] = { "A's", "ABC", "ACT", "AD", "AFC", "Abbe",
          "Abbye's", "Abelard", "Abernathy's", "Abidjan", "Abie", "Aborigines",
          "Acadia", "Acadia's", "Adas", "Adella's", "Adena's", "Adey's",
          "Adham", "Ado's", "Adorne", "Adrianne", "Adriena", "Aeneas",
          "Aeneid's", "Africa", "Ag's", "Agace", "Agana's", "Aggie", "Agnes's",
          "Aguascalientes's", "Agustin's", "Aida", "Aida's", "Aila's", "Ailina",
          "Ajay's", "Akihito", "Aksel's", "Alabama's", "Alaine", "Alanson's",
          "Alaska", "Alasteir", "Alaster", "Alayne's", "Albany", "Albertine's",
          "Albrecht's", "Alcibiades's", "Alcuin", "Alec", "Alejandro", "Alex's",
          "Alexander", "Alexandra's", "Alexandria", "Alexio", "Alfons",
          "Alfonso's", "Algonquian", "Algonquian's", "Algonquin", "Alie",
          "Alissa", "Allah's", "Allard's", "Alleen", "Allegheny", "Allianora",
          "Allianora's", "Allstate", "Allyn", "Almach's", "Almeria",
          "Almoravid", "Aloise's", "Althea", "Altman's", "Alvina", "Alvina's",
          "Alvy", "Alyson", "Amalea", "Amalita", "Ambrosius's", "Amelie's",
          "Amery's", "Amoco", "Anabel", "Analects's", "Analise", "Anaxagoras's",
          "Anchorage", "Andaman", "Andie", "Andie's", "Andorra", "Andorra's",
          "Andorrans", "Andre", "Andre's", "Andrej", "Andrew", "Andropov",
          "Angela", "Angles", "Anglos", "Angora's", "Angus", "Angy", "Ann",
          "Ann's", "Annabella's", "Annadiana's", "Annette", "Annnora's",
          "Annunciations", "Ansell's", "Anselma's", "Anshan", "Antananarivo's",
          "Anthiathia", "Antillean", "Antonius", "Antwerp", "Antwerp's",
          "Aphrodite", "Apocalypse", "Apollinaire", "Apr", "Aquarius's",
          "Aquariuses", "Ar's", "Arabella's", "Arabist's", "Araguaya",
          "Aramco's", "Arapaho's", "Arawakan", "Archer", "Archibold", "Ardabil",
          "Ardelis's", "Ardenia's", "Argentine", "Argentinian", "Ari",
          "Arianism's", "Ario's", "Arius's", "Arizona", "Arliene's",
          "Arlington's", "Arnaldo's", "Arneb's", "Arron's", "Arte's", "Arther",
          "Arv's", "Arvie's", "Asa's", "Ashe's", "Asher's", "Ashli's",
          "Ashlin's", "Ashmolean's", "Assam's", "Astana", "Astor", "At's",
          "Athens", "Atlante", "Atman", "Attica's", "Attila", "Aubree's",
          "Aubrie", "Audrie's", "Audy", "Augy", "Aurelie's", "Aussie",
          "Australasian", "Ava", "Avalon", "Aveline's", "Avernus's", "Aves",
          "Avesta", "Avrom's", "Axe", "Aymara", "Azana", "Azania's", "BMW's",
          "BO", "Bab", "Babylons", "Bactria's", "Baha'i", "Bahamians",
          "Bailie's", "Bale", "Bank", "Baotou's", "Baptiste", "Barbarossa's",
          "Barbe's", "Barde", "Barker", "Barr's", "Barrett's", "Barthel's",
          "Bartolomeo's", "Bary's", "Basutoland", "Bates", "Baywatch",
          "Bearnaise's", "Beasley's", "Beatlemania", "Beatrix", "Beauvoir's",
          "Beckie", "Beerbohm", "Behan", "Bekesy's", "Belfast's", "Belize",
          "Bellanca's", "Belva's", "Ben", "Benedetta", "Benedick's",
          "Benedicta's", "Benetta", "Bengal", "Benghazi", "Benito's", "Benz",
          "Ber's", "Bergman", "Berk", "Berkeley's", "Berlins", "Bernelle",
          "Bernelle's", "Bert", "Berte's", "Bertie's", "Bertillon", "Berton",
          "Beryl", "Bessemer", "Bethesda's", "Betta", "Bettina", "Bevan's",
          "Beverlie's", "Bevvy's", "Bhutan", "Bilbao's", "Billie's", "Bimini's",
          "Birk's", "Bismarck", "Bismark", "Bjorn", "Blair's", "Blanca",
          "Bluebeard's", "Bobbie's", "Bobbye", "Boccaccio's", "Bolshevist",
          "Bonner", "Bonni's", "Booth", "Bordeaux", "Bordeaux's", "Borg's",
          "Borlaug's", "Borneo's", "Bourbons", "Bowie's", "Boy's", "Boyle",
          "Brad", "Bradburys", "Bradley", "Brahmagupta", "Brahmagupta's",
          "Brana", "Branch", "Brander", "Brandie", "Brando", "Brandtr",
          "Brasilia", "Breanne's", "Brear", "Brena's", "Brendin's", "Brenn",
          "Brennen", "Briana's", "Bridgeport's", "Bridie", "Bries", "Brillo",
          "Brinkley's", "Brion's", "Brisbane", "Briticisms", "British",
          "Britni", "Broadway's", "Bron", "Bronson's", "Bronte", "Bronx",
          "Bros", "Brown", "Brownie's", "Brunei's", "Bryant", "Bryon",
          "Bucharest", "Buchenwald's", "Buckner", "Budd's", "Buddha's",
          "Buford", "Bujumbura's", "Bukhara", "Bultmann", "Burke's", "Burl's",
          "Burnaby", "Bursa's", "Burundians", "Butch", "Butch's",
          "Byelorussia's", "CA", "CD", "COBOLs", "COD", "COL", "Cabernet's",
          "Cabrera's", "Cad", "Caddric", "Cadillac", "Cadiz", "Caedmon", "Cage",
          "Cains", "Cairo", "Caitrin", "Calif's", "Caligula", "Calla",
          "Callahan's", "Calley", "Calli's", "Calliope", "Calvinistic",
          "Camala's", "Camembert", "Camembert's", "Camus's", "Canadianism",
          "Canaletto", "Cancer", "Cannon", "Cantonese's", "Cantor", "Capella",
          "Caph's", "Capitol", "Capote's", "Cardin", "Caressa's", "Carie's",
          "Carilyn", "Carlene", "Carlos", "Carlton's", "Carlyle's", "Carlyn's",
          "Carmelo", "Carolann's", "Carpenter", "Carrie's", "Carrier's",
          "Carry's", "Cary", "Casper's", "Castlereagh", "Castries's", "Catalan",
          "Caterina", "Cathrin's", "Cathy", "Cati's", "Catriona's", "Celene",
          "Celka's", "Cello", "Ceres", "Cerf's", "Cessna", "Chaitanya's",
          "Chaitin's", "Chaldea's", "Chamberlain's", "Champollion's",
          "Chantilly", "Charissa", "Charisse's", "Charla", "Charlot's",
          "Charon's", "Chartism's", "Chattanooga's", "Chayefsky's",
          "Chelyabinsk's", "Chere's", "Cherise's", "Chernenko", "Cherry's",
          "Cheryl's", "Cheston's", "Chibcha", "Chicana", "Chicky", "Chico",
          "Chippendale's", "Chisholm's", "Chou's", "Chretien's", "Christiane",
          "Christians", "Christoforo", "Christoph's", "Chronicles", "Chrotoem",
          "Chumash's", "Churchill", "Cicero", "Cilka's", "Cinnamon", "Ciro",
          "Clair's", "Clara", "Clarabelle", "Claudie's", "Claudius's",
          "Clausewitz's", "Clausius", "Clay's", "Clea's", "Cleavland's",
          "Clement's", "Clemmy", "Clerissa's", "Cleveland", "Clouseau's", "Cly",
          "Cmdr", "Cobb's", "Cochin's", "Coffey", "Coleen", "Coleman",
          "Colet's", "Colgate's", "Colin", "Collen's", "Colorado", "Coloradoan",
          "Columbia", "Columbine", "Comdr's", "Comintern's", "Communists",
          "Como's", "Concepcion", "Congress's", "Conrade's", "Conroy",
          "Consolata's", "Constantinople", "Continent's", NULL };
        
        for(int i = 0; true; i++) {
          if (!data[i])
            break;
          //out << "insert: " << data[i] << std::endl;
          db->find(data[i]);
          db->set_value(value(1));
        }
      }
};


TestMemoryDB test_mdb;

BOOST_AUTO_TEST_CASE(Access) {
  test_mdb.test_Access();
}

BOOST_AUTO_TEST_CASE(Random) {
  test_mdb.test_Random();
}


#ifdef BOOST_TEST_NO_MAIN
int main(int argc, const char* argv[]) {
  //test_mdb.test_Access();
  test_mdb.test_Random();
  return 0;
}

#if 0
// set_terminate example
#include <iostream>       // std::cerr
#include <exception>      // std::set_terminate
#include <cstdlib>        // std::abort
#include <unistd.h>
#include <signal.h>


void segsev(int) {
  std::cerr << "segmentation fault: "<< getpid() << "\n";
  sleep(10000);
}

void myterminate () {
  std::cerr << "terminate handler called: "<< getpid() << "\n";
  abort();  // forces abnormal termination
}


struct SetTerminate {
  SetTerminate() {
    std::set_terminate(myterminate);
    signal(SIGSEGV, segsev);
  }
};

SetTerminate s;
#endif
#endif

