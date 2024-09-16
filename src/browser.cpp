// declarations for the memory mapped storage
#ifndef _LEAVES_BROWSER_CPP
#define _LEAVES_BROWSER_CPP

#include "browser.hpp"


namespace leaves {


DBBrowser::DBBrowser() {

}
  
DBBrowser::~DBBrowser() {

}

size_t DBBrowser::DBBrowser::get_size() const { 
    
 }

block_ptr DBBrowser::get_block(offset_ptr offset) const {

}
  
block_ptr DBBrowser::alloc_cow_block(tid_t min_txn_id) {

}

block_ptr DBBrowser::get_txn_block(offset_ptr offset) {

}

block_ptr DBBrowser::clone_cow_block(tid_t min_txn_id, offset_ptr offset) {

}

offset_ptr DBBrowser::alloc_block(tid_t min_txn_id, int poolid) {

}

offset_ptr DBBrowser::alloc_new_block(int pool_id) {

}

void DBBrowser::free_cow_block(BlockUnion* block) {

}

void DBBrowser::free_block(block_ptr block, int pool_id) {

}

const DBTransaction* DBBrowser::get_active_txn() const {

}

const BlockUnion* DBBrowser::get_root() const {

}

bool DBBrowser::start_transaction() {

}

void DBBrowser::rollback() {


}

void DBBrowser::prepare_commit() {

}

void DBBrowser::commit() {

}

void DBBrowser::end_transaction() {

}

int DBBrowser::alloc_cursor() {

}

offset_ptr DBBrowser::update_cursor(int id) {

}

void DBBrowser::free_cursor(int id) {

}




}  // namespace leaves

#endif  // _LEAVES_BROWSER_CPP
