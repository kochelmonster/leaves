
// #include <leaves/db.hpp>

#include <iostream>

#include "../include/leaves/mmap.hpp"

using namespace leaves;

#define BOOST_TEST_MODULE StringInsertTest

#include <stdlib.h>

#include <algorithm>
#include <boost/test/included/unit_test.hpp>
#include <ostream>

#include "test.hpp"

#undef SEGMENT_SIZE
#define SEGMENT_SIZE 1024 * 1024 * 4

const char* names[] = {"A's",
                       "ABC",
                       "ACT",
                       "AD",
                       "AFC",
                       "Abbe",
                       "Abbye's",
                       "Abelard",
                       "Abernathy's",
                       "Abidjan",
                       "Abie",
                       "Aborigines",
                       "Acadia",
                       "Acadia's",
                       "Adas",
                       "Adella's",
                       "Adena's",
                       "Adey's",
                       "Adham",
                       "Ado's",
                       "Adorne",
                       "Adrianne",
                       "Adriena",
                       "Aeneas",
                       "Aeneid's",
                       "Africa",
                       "Ag's",
                       "Agace",
                       "Agana's",
                       "Aggie",
                       "Agnes's",
                       "Aguascalientes's",
                       "Agustin's",
                       "Aida",
                       "Aida's",
                       "Aila's",
                       "Ailina",
                       "Ajay's",
                       "Akihito",
                       "Aksel's",
                       "Alabama's",
                       "Alaine",
                       "Alanson's",
                       "Alaska",
                       "Alasteir",
                       "Alaster",
                       "Alayne's",
                       "Albany",
                       "Albertine's",
                       "Albrecht's",
                       "Alcibiades's",
                       "Alcuin",
                       "Alec",
                       "Alejandro",
                       "Alex's",
                       "Alexander",
                       "Alexandra's",
                       "Alexandria",
                       "Alexio",
                       "Alfons",
                       "Alfonso's",
                       "Algonquian",
                       "Algonquian's",
                       "Algonquin",
                       "Alie",
                       "Alissa",
                       "Allah's",
                       "Allard's",
                       "Alleen",
                       "Allegheny",
                       "Allianora",
                       "Allianora's",
                       "Allstate",
                       "Allyn",
                       "Almach's",
                       "Almeria",
                       "Almoravid",
                       "Aloise's",
                       "Althea",
                       "Altman's",
                       "Alvina",
                       "Alvina's",
                       "Alvy",
                       "Alyson",
                       "Amalea",
                       "Amalita",
                       "Ambrosius's",
                       "Amelie's",
                       "Amery's",
                       "Amoco",
                       "Anabel",
                       "Analects's",
                       "Analise",
                       "Anaxagoras's",
                       "Anchorage",
                       "Andaman",
                       "Andie",
                       "Andie's",
                       "Andorra",
                       "Andorra's",
                       "Andorrans",
                       "Andre",
                       "Andre's",
                       "Andrej",
                       "Andrew",
                       "Andropov",
                       "Angela",
                       "Angles",
                       "Anglos",
                       "Angora's",
                       "Angus",
                       "Angy",
                       "Ann",
                       "Ann's",
                       "Annabella's",
                       "Annadiana's",
                       "Annette",
                       "Annnora's",
                       "Annunciations",
                       "Ansell's",
                       "Anselma's",
                       "Anshan",
                       "Antananarivo's",
                       "Anthiathia",
                       "Antillean",
                       "Antonius",
                       "Antwerp",
                       "Antwerp's",
                       "Aphrodite",
                       "Apocalypse",
                       "Apollinaire",
                       "Apr",
                       "Aquarius's",
                       "Aquariuses",
                       "Ar's",
                       "Arabella's",
                       "Arabist's",
                       "Araguaya",
                       "Aramco's",
                       "Arapaho's",
                       "Arawakan",
                       "Archer",
                       "Archibold",
                       "Ardabil",
                       "Ardelis's",
                       "Ardenia's",
                       "Argentine",
                       "Argentinian",
                       "Ari",
                       "Arianism's",
                       "Ario's",
                       "Arius's",
                       "Arizona",
                       "Arliene's",
                       "Arlington's",
                       "Arnaldo's",
                       "Arneb's",
                       "Arron's",
                       "Arte's",
                       "Arther",
                       "Arv's",
                       "Arvie's",
                       "Asa's",
                       "Ashe's",
                       "Asher's",
                       "Ashli's",
                       "Ashlin's",
                       "Ashmolean's",
                       "Assam's",
                       "Astana",
                       "Astor",
                       "At's",
                       "Athens",
                       "Atlante",
                       "Atman",
                       "Attica's",
                       "Attila",
                       "Aubree's",
                       "Aubrie",
                       "Audrie's",
                       "Audy",
                       "Augy",
                       "Aurelie's",
                       "Aussie",
                       "Australasian",
                       "Ava",
                       "Avalon",
                       "Aveline's",
                       "Avernus's",
                       "Aves",
                       "Avesta",
                       "Avrom's",
                       "Axe",
                       "Aymara",
                       "Azana",
                       "Azania's",
                       "BMW's",
                       "BO",
                       "Bab",
                       "Babylons",
                       "Bactria's",
                       "Baha'i",
                       "Bahamians",
                       "Bailie's",
                       "Bale",
                       "Bank",
                       "Baotou's",
                       "Baptiste",
                       "Barbarossa's",
                       "Barbe's",
                       "Barde",
                       "Barker",
                       "Barr's",
                       "Barrett's",
                       "Barthel's",
                       "Bartolomeo's",
                       "Bary's",
                       "Basutoland",
                       "Bates",
                       "Baywatch",
                       "Bearnaise's",
                       "Beasley's",
                       "Beatlemania",
                       "Beatrix",
                       "Beauvoir's",
                       "Beckie",
                       "Beerbohm",
                       "Behan",
                       "Bekesy's",
                       "Belfast's",
                       "Belize",
                       "Bellanca's",
                       "Belva's",
                       "Ben",
                       "Benedetta",
                       "Benedick's",
                       "Benedicta's",
                       "Benetta",
                       "Bengal",
                       "Benghazi",
                       "Benito's",
                       "Benz",
                       "Ber's",
                       "Bergman",
                       "Berk",
                       "Berkeley's",
                       "Berlins",
                       "Bernelle",
                       "Bernelle's",
                       "Bert",
                       "Berte's",
                       "Bertie's",
                       "Bertillon",
                       "Berton",
                       "Beryl",
                       "Bessemer",
                       "Bethesda's",
                       "Betta",
                       "Bettina",
                       "Bevan's",
                       "Beverlie's",
                       "Bevvy's",
                       "Bhutan",
                       "Bilbao's",
                       "Billie's",
                       "Bimini's",
                       "Birk's",
                       "Bismarck",
                       "Bismark",
                       "Bjorn",
                       "Blair's",
                       "Blanca",
                       "Bluebeard's",
                       "Bobbie's",
                       "Bobbye",
                       "Boccaccio's",
                       "Bolshevist",
                       "Bonner",
                       "Bonni's",
                       "Booth",
                       "Bordeaux",
                       "Bordeaux's",
                       "Borg's",
                       "Borlaug's",
                       "Borneo's",
                       "Bourbons",
                       "Bowie's",
                       "Boy's",
                       "Boyle",
                       "Brad",
                       "Bradburys",
                       "Bradley",
                       "Brahmagupta",
                       "Brahmagupta's",
                       "Brana",
                       "Branch",
                       "Brander",
                       "Brandie",
                       "Brando",
                       "Brandtr",
                       "Brasilia",
                       "Breanne's",
                       "Brear",
                       "Brena's",
                       "Brendin's",
                       "Brenn",
                       "Brennen",
                       "Briana's",
                       "Bridgeport's",
                       "Bridie",
                       "Bries",
                       "Brillo",
                       "Brinkley's",
                       "Brion's",
                       "Brisbane",
                       "Briticisms",
                       "British",
                       "Britni",
                       "Broadway's",
                       "Bron",
                       "Bronson's",
                       "Bronte",
                       "Bronx",
                       "Bros",
                       "Brown",
                       "Brownie's",
                       "Brunei's",
                       "Bryant",
                       "Bryon",
                       "Bucharest",
                       "Buchenwald's",
                       "Buckner",
                       "Budd's",
                       "Buddha's",
                       "Buford",
                       "Bujumbura's",
                       "Bukhara",
                       "Bultmann",
                       "Burke's",
                       "Burl's",
                       "Burnaby",
                       "Bursa's",
                       "Burundians",
                       "Butch",
                       "Butch's",
                       "Byelorussia's",
                       "CA",
                       "CD",
                       "COBOLs",
                       "COD",
                       "COL",
                       "Cabernet's",
                       "Cabrera's",
                       "Cad",
                       "Caddric",
                       "Cadillac",
                       "Cadiz",
                       "Caedmon",
                       "Cage",
                       "Cains",
                       "Cairo",
                       "Caitrin",
                       "Calif's",
                       "Caligula",
                       "Calla",
                       "Callahan's",
                       "Calley",
                       "Calli's",
                       "Calliope",
                       "Calvinistic",
                       "Camala's",
                       "Camembert",
                       "Camembert's",
                       "Camus's",
                       "Canadianism",
                       "Canaletto",
                       "Cancer",
                       "Cannon",
                       "Cantonese's",
                       "Cantor",
                       "Capella",
                       "Caph's",
                       "Capitol",
                       "Capote's",
                       "Cardin",
                       "Caressa's",
                       "Carie's",
                       "Carilyn",
                       "Carlene",
                       "Carlos",
                       "Carlton's",
                       "Carlyle's",
                       "Carlyn's",
                       "Carmelo",
                       "Carolann's",
                       "Carpenter",
                       "Carrie's",
                       "Carrier's",
                       "Carry's",
                       "Cary",
                       "Casper's",
                       "Castlereagh",
                       "Castries's",
                       "Catalan",
                       "Caterina",
                       "Cathrin's",
                       "Cathy",
                       "Cati's",
                       "Catriona's",
                       "Celene",
                       "Celka's",
                       "Cello",
                       "Ceres",
                       "Cerf's",
                       "Cessna",
                       "Chaitanya's",
                       "Chaitin's",
                       "Chaldea's",
                       "Chamberlain's",
                       "Champollion's",
                       "Chantilly",
                       "Charissa",
                       "Charisse's",
                       "Charla",
                       "Charlot's",
                       "Charon's",
                       "Chartism's",
                       "Chattanooga's",
                       "Chayefsky's",
                       "Chelyabinsk's",
                       "Chere's",
                       "Cherise's",
                       "Chernenko",
                       "Cherry's",
                       "Cheryl's",
                       "Cheston's",
                       "Chibcha",
                       "Chicana",
                       "Chicky",
                       "Chico",
                       "Chippendale's",
                       "Chisholm's",
                       "Chou's",
                       "Chretien's",
                       "Christiane",
                       "Christians",
                       "Christoforo",
                       "Christoph's",
                       "Chronicles",
                       "Chrotoem",
                       "Chumash's",
                       "Churchill",
                       "Cicero",
                       "Cilka's",
                       "Cinnamon",
                       "Ciro",
                       "Clair's",
                       "Clara",
                       "Clarabelle",
                       "Claudie's",
                       "Claudius's",
                       "Clausewitz's",
                       "Clausius",
                       "Clay's",
                       "Clea's",
                       "Cleavland's",
                       "Clement's",
                       "Clemmy",
                       "Clerissa's",
                       "Cleveland",
                       "Clouseau's",
                       "Cly",
                       "Cmdr",
                       "Cobb's",
                       "Cochin's",
                       "Coffey",
                       "Coleen",
                       "Coleman",
                       "Colet's",
                       "Colgate's",
                       "Colin",
                       "Collen's",
                       "Colorado",
                       "Coloradoan",
                       "Columbia",
                       "Columbine",
                       "Comdr's",
                       "Comintern's",
                       "Communists",
                       "Como's",
                       "Concepcion",
                       "Congress's",
                       "Conrade's",
                       "Conroy",
                       "Consolata's",
                       "Constantinople",
                       "Continent's",
                       NULL};

template <typename content_t>
std::string value(content_t content, size_t size = 0) {
  std::stringstream f;
  f << "v:" << content;
  std::string result(f.str());
  while (result.size() < size) result.push_back('-');

  return result;
}

std::string number(int number, size_t size = 0) {
  std::stringstream f;
  f << std::setw(size) << std::setfill('0') << number;
  return f.str();
}

typedef MapStorage Storage;
// typedef _MemoryMapFile<_MemoryMapTraits> Storage;

BOOST_AUTO_TEST_CASE(test_strings) {
  Preparation p;
  {
    auto storage = Storage::create(TEST_FILE);
  }

  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();

  std::ostream null_stream(nullptr);
  size_t count;
  for (count = 0; names[count] && count < 1000; count++) {
    std::cout << "insert: " << count << ". " << names[count] << std::endl;
    if (count == 401) {
      std::cout << "wrong" << std::endl;
    }

    cursor.find(names[count]);
    BOOST_REQUIRE(!cursor.is_valid());
    // cursor->set_value(value(count, 900));
    cursor.value(value(count, 10));
    /*if (leaves::dump_db(null_stream, db) != count+1) {
      std::cerr << "error!" << std::endl;
      break;
    }*/

    if (false) {
      std::stringstream cstr;
      cstr << "errors/test_" << std::setw(2) << std::setfill('0') << count
           << ".yaml";
      std::ofstream out(cstr.str().c_str());
      _Dumper(db, &db._internal()->_wtxn->root, false).dump(out);
    }
    if (count % 20 == 0 && count > 0) {
      cursor.commit();
    }
  }
  cursor.commit();
  // _MemoryChecker<Storage>(storage).check();

  std::stringstream cstr;
  cstr << "errors/test_" << std::setw(2) << std::setfill('0') << count
       << ".yaml";
  std::ofstream out(cstr.str().c_str());
  _Dumper(db, &db._internal()->txn()->root, false).dump(out);

  std::cout << "start test: " << count << std::endl;
  for (int i = 0; i < 100; i++) {
    int rand_int = rand() % count;
    const char* name = names[rand_int];
    std::cout << "test " << name << " (" << rand_int << ")" << std::endl;
    cursor.find(name);
    BOOST_REQUIRE(cursor.is_valid());
    BOOST_REQUIRE_EQUAL(cursor.key().string(), std::string(name));
  }
}

BOOST_AUTO_TEST_CASE(test_numbers) {
  Preparation p;
  auto storage = Storage::create(TEST_FILE);
  auto db = (*storage)["test"];
  auto cursor = db.cursor();
  int i;

  for (i = 0; i < 10000; i += 2) {
    std::string n = number(i, 6);
    cursor.find(n);
    cursor.value(n);
  }
  cursor.commit();

  for (cursor.first(), i = 0; cursor.is_valid(); cursor.next(), i += 2) {
    std::string n = number(i, 6);
    BOOST_REQUIRE_EQUAL(cursor.key().string(), n);
    BOOST_REQUIRE_EQUAL(cursor.value().string(), n);
  }
  BOOST_REQUIRE_EQUAL(i, 10000);

  for (cursor.last(), i = 10000 - 2; cursor.is_valid(); cursor.prev(), i -= 2) {
    std::string n = number(i, 6);
    BOOST_REQUIRE_EQUAL(cursor.key().string(), n);
    BOOST_REQUIRE_EQUAL(cursor.value().string(), n);
  }
  BOOST_REQUIRE_EQUAL(i, -2);

  std::string n = number(1000, 6);
  cursor.find(n);
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key().string(), n);
  BOOST_REQUIRE_EQUAL(cursor.value().string(), n);

  n = number(1001, 6);
  cursor.find(n);
  BOOST_REQUIRE(!cursor.is_valid());
  cursor.next();
  n = number(1002, 6);
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key().string(), n);
  BOOST_REQUIRE_EQUAL(cursor.value().string(), n);

  n = number(1001, 6);
  cursor.find(n);
  BOOST_REQUIRE(!cursor.is_valid());
  cursor.prev();
  n = number(1000, 6);
  BOOST_REQUIRE(cursor.is_valid());
  BOOST_REQUIRE_EQUAL(cursor.key().string(), n);
  BOOST_REQUIRE_EQUAL(cursor.value().string(), n);

  for (cursor.first(); cursor.is_valid(); cursor.first())
    cursor.remove();
  cursor.commit();

  cursor.first();
  BOOST_REQUIRE(!cursor.is_valid());
}
