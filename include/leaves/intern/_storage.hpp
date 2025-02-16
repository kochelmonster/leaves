#ifndef _LEAVES__STORAGE_HPP
#define _LEAVES__STORAGE_HPP

template<typename MEM_MANAGER, typename TXN_MANAGER>
struct Storage {

  


  MEM_MANAGER _memory;
  TXN_MANAGER _txn_manager;
};

#endif // _LEAVES__STORAGE_HPP
