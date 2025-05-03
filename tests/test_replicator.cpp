#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE TrieNodeTest

#include <boost/test/included/unit_test.hpp>

#ifndef TESTING
#error "TESTING must be defined"
#endif

#include "leaves/intern/_replicatior.hpp"

struct DirPreparation {
  DirPreparation() {
    tempDir = std::filesystem::temp_directory_path() / "test_db";
    ::std::filesystem::remove_all(tempDir);
    std::filesystem::create_directory(tempDir);
    std::filesystem::path dbFilePath = tempDir / "test.lvs";
  }

  ~DirPreparation() { std::filesystem::remove_all(tempDir); }

  std::filesystem::path tempDir;
};

typedef _MemoryMapFile<_MemoryMapTraits> DBMMap;
typedef _Replicator<DBMMap> Replicator;

void replicate(Replicator& replicator1, Replicator& replicator2) {
  while (true) {
    int wait = 0;

    switch (replicator1.exec()) {
      case Replicator::START_SEND:
        if (replicator1.ready_for_sending() == Slice("test1"))
          replicator1.continue_();
        else
          replicator1.cancel();
        break;

      case Replicator::START_RECEIVE:
        if (replicator1.ready_for_receiving() == Slice("test1"))
          replicator1.cancel();
        else
          replicator1.continue_();
        break;

      case Replicator::SEND:
        replicator2.receive(replicator1.send());
        break;

      case Replicator::RECEIVE:
        // wait data from peer
        break;

      case Replicator::WAIT:
        // nothing to do
        wait++;
        break;
    }
  }

  switch (replicator2.exec()) {
    case Replicator::START_SEND:
      replicator2.continue_();
      break;

    case Replicator::START_RECEIVE:
      replicator2.continue_();
      break;

    case Replicator::SEND:
      replicator1.receive(replicator2.send());
      break;

    case Replicator::RECEIVE:
      // wait data from peer
      break;

    case Replicator::WAIT:
      // nothing to do
      wait++;
      break;
  }

  if (wait == 2) break;  // both parties finished working
}

BOOST_AUTO_TEST_CASE(test_replicator) {
  std::filesystem::path dbFilePath = prep.tempDir / "test.lvs";
  DBMMap storage(dbFilePath.c_str());
  Replicator replicator1(storage);
  Replicator replicator2(storage);

  auto db = storage["test1"];
  auto cursor1 = db->cursor();

  auto db = storage["test2"];
  auto cursor2 = db->cursor();

  cursor1.find("abc");
  cursor1.value("abc");
  cursor1.commit();

  replicator1.start();
  replicate(replicator1, replicator2);


  cursor.find("abc");
  BOOST_CHECK(cursor.is_valid());


  /*
    Replicator.start
        - änderung des storage
        - eingang einer message


  */
}
