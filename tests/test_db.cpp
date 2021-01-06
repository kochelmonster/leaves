#define BOOST_TEST_MODULE DBTest

#include <stdlib.h>
#include <algorithm>
#include <boost/test/included/unit_test.hpp>
#include "test.hpp"


const char* names[] = { "A's", "ABC", "ACT", "AD", "AFC", "Abbe",
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


namespace leaves {
void dump_db(std::ostream& out, DB::ptr db);
}


template<typename content_t> std::string
value(content_t content, size_t size=0) {
  std::stringstream f;
  f << "value-" << content;
  std::string result(f.str());
  while(result.size() < size)
    result.push_back('-');

  return result;
}

std::string number(int number, size_t size=0) {
  std::stringstream f;
  f << std::setw(size) << std::setfill('0') << number;
  return f.str();
}


BOOST_AUTO_TEST_CASE(test_strings) {
  Preparation p;

  DB::ptr db(DB::open(TEST_FILE, TEST_OPTIONS));
  DB::cursor_ptr cursor(db->create_cursor());

  std::cout << "refcount" << db.use_count() << std::endl;

  size_t count;
  for(count = 0; names[count]; count++) {
    std::cout << "insert: " << count << ". " << names[count] << std::endl;

    cursor->find(names[count]);
    BOOST_REQUIRE(!cursor->valid());
    cursor->set_value(value(names[count]));

    if (count == 154)
    std::cout << "dump: " << count << ". " << names[count] << std::endl;

    /*
    std::stringstream cstr;
    cstr << "errors/test_" << std::setw(2) << std::setfill('0') << count << ".yaml";
    std::ofstream out(cstr.str().c_str());
    dump_db(out, db);*/
  }

  Stats stats;
  db->get_stats(stats);
  std::cout << "node statistics" << std::endl;
  for(int i = 0; i < 5; i++) {
    std::cout << "used_nodes  ["<<i<<"]:   " << stats.used_nodes[i] << std::endl;
    std::cout << "freed_nodes ["<<i<<"]:   " << stats.freed_nodes[i] << std::endl;
  }

  std::cout << "start find test: " << count << std::endl;
  for(int i = 0; i < 100; i++) {
    const char* name = names[rand() % count];
    std::cout << "test " << name << std::endl;
    cursor->find(name);
    BOOST_REQUIRE(cursor->valid());
    BOOST_REQUIRE_EQUAL(cursor->key().string(), std::string(name));
  }

  std::cout << "start missing test: " << count << std::endl;
  for(int i = 0; i < 100; i++) {
    const char* name = names[rand() % count];
    std::string src(name);
    src.append("--no");
    std::cout << "no test " << src << std::endl;
    cursor->find(src);
    BOOST_REQUIRE(!cursor->valid());
  }
}


BOOST_AUTO_TEST_CASE(test_numbers) {
  Preparation p;
  int i;
  DB::ptr db(DB::open(TEST_FILE, TEST_OPTIONS));
  DB::cursor_ptr cursor(db->create_cursor());


  for(i = 0; i < 10000; i+=2) {
    std::string n = number(i, 6);
    cursor->find(n);
    //std::cout << "insert " << i <<  std::endl;
    cursor->set_value(n);
  }
  std::cout << "generated! " << i << std::endl;

  for(cursor->first(), i = 0; cursor->valid(); cursor->next(), i+=2) {
    std::string n = number(i, 6);
    BOOST_REQUIRE(cursor->key() == n);
    BOOST_REQUIRE(cursor->value() == n);
  }

  BOOST_REQUIRE(i == 10000);
  std::cout << "forward iteration passed" << std::endl;

  for(cursor->last(), i = 10000-2; cursor->valid(); cursor->prev(), i-=2) {
    std::string n = number(i, 6);
    BOOST_REQUIRE(cursor->key() == n);
    BOOST_REQUIRE(cursor->value() == n);
  }

  BOOST_REQUIRE(i == -2);
  std::cout << "backward iteration passed" << std::endl;

  std::string n = number(1000, 6);
  cursor->find(n);
  BOOST_REQUIRE(cursor->valid());
  BOOST_REQUIRE(cursor->key() == n);
  BOOST_REQUIRE(cursor->value() == n);

  n = number(1001, 6);
  cursor->find(n);
  BOOST_REQUIRE(!cursor->valid());
  cursor->next();
  n = number(1002, 6);
  BOOST_REQUIRE(cursor->valid());
  BOOST_REQUIRE(cursor->key() == n);
  BOOST_REQUIRE(cursor->value() == n);

  n = number(1001, 6);
  cursor->find(n);
  BOOST_REQUIRE(!cursor->valid());
  cursor->prev();
  n = number(1000, 6);
  std::cout << "smaller than 1001: " << cursor->key().string() << std::endl;
  BOOST_REQUIRE(cursor->valid());
  BOOST_REQUIRE(cursor->key() == n);
  BOOST_REQUIRE(cursor->value() == n);
  std::cout << "find passed" << std::endl;

  for(cursor->first(); cursor->valid(); cursor->first()) {
    cursor->remove();
  }

  std::cout << "removed passed" << std::endl;
  for(cursor->first(); cursor->valid(); cursor->next())
    BOOST_REQUIRE(0);
}
