#ifndef LEAVES_TRANSACTION_MONITOR_HPP
#define LEAVES_TRANSACTION_MONITOR_HPP

#include <leaves/mmap.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct TransactionMonitor {
  using Storage = leaves::MapStorage;
  using Database = Storage::DB;
  using Cursor = Database::Cursor;

  enum class State : uint8_t {
    none,
    preparing,
    aborting,
    committing,
    aborted,
    committed,
  };

  enum class Progress { pending, complete };
  using PersistHook = std::function<void(State)>;

  TransactionMonitor(Storage::storage_ptr storage, std::string transaction_id,
                     std::string coordinator_database =
                         "_transaction_monitor",
                     PersistHook persist_hook = {})
      : storage_(std::move(storage)),
        coordinator_database_(storage_->open(coordinator_database)),
        coordinator_cursor_(coordinator_database_.cursor()),
        transaction_id_(std::move(transaction_id)),
        persist_hook_(std::move(persist_hook)) {
    recover_coordinator_database();
    load();
  }

  void add_participant(std::string name, Database& database, Cursor& cursor) {
    if (std::any_of(participants_.begin(), participants_.end(),
                    [&name](const Participant& participant) {
                      return participant.name == name;
                    })) {
      throw std::invalid_argument("duplicate transaction participant: " +
                                  name);
    }

    Participant participant{std::move(name), &database, &cursor,
                            leaves::tid_t(0), false};
    auto persisted = std::find_if(
        persisted_participants_.begin(), persisted_participants_.end(),
        [&participant](const PersistedParticipant& stored) {
          return stored.name == participant.name;
        });

    if (state_ != State::none && persisted == persisted_participants_.end()) {
      throw std::invalid_argument(
          "missing persisted transaction participant: " + participant.name);
    }
    if (persisted != persisted_participants_.end()) {
      participant.transaction_id = persisted->transaction_id;
      participant.finished = persisted->finished;
    }
    participants_.push_back(std::move(participant));
  }

  State state() const { return state_; }

  bool start() {
    validate_participants();
    if (state_ != State::none) {
      return false;
    }
    for (const Participant& participant : participants_) {
      if (participant.cursor->is_transaction_active()) {
        return false;
      }
    }

    std::vector<Cursor*> started;
    started.reserve(participants_.size());
    try {
      for (Participant& participant : participants_) {
        if (!participant.cursor->start_transaction()) {
          rollback_started(started);
          return false;
        }
        started.push_back(participant.cursor);
      }
    } catch (...) {
      rollback_started(started);
      throw;
    }
    return true;
  }

  Progress commit() {
    validate_participants();

    if (state_ == State::committed || state_ == State::aborted) {
      return Progress::complete;
    }

    if (state_ == State::none) {
      state_ = State::preparing;
      persist();

      for (Participant& participant : participants_) {
        try {
          participant.transaction_id =
              participant.cursor->prepare_commit(true);
        } catch (...) {
          begin_abort();
          return finish_abort();
        }

        if (participant.transaction_id == leaves::tid_t(0)) {
          begin_abort();
          return finish_abort();
        }
        persist();
      }

      state_ = State::committing;
      persist();
    } else if (state_ == State::preparing) {
      // No durable commit decision exists. Presumed-abort is mandatory.
      begin_abort();
    }

    if (state_ == State::aborting) {
      return finish_abort();
    }
    return finish_commit();
  }

  Progress rollback() {
    validate_participants();
    if (state_ == State::committing || state_ == State::committed) {
      throw std::logic_error("cannot roll back a committed transaction");
    }
    if (state_ == State::aborted) {
      return Progress::complete;
    }
    if (state_ != State::aborting) {
      begin_abort();
    }
    return finish_abort();
  }

  Progress resolve() { return commit(); }

  struct Participant {
    std::string name;
    Database* database;
    Cursor* cursor;
    leaves::tid_t transaction_id;
    bool finished;
  };

  struct PersistedParticipant {
    std::string name;
    leaves::tid_t transaction_id;
    bool finished;
  };

  static constexpr char kMagic[] = "LTM1";

  static void append_u32(std::string& value, uint32_t number) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      value.push_back(static_cast<char>((number >> shift) & 0xff));
    }
  }

  static uint32_t read_u32(const std::string& value, size_t& offset) {
    if (offset > value.size() || value.size() - offset < sizeof(uint32_t)) {
      throw std::runtime_error("truncated transaction monitor record");
    }

    uint32_t number = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      number |= static_cast<uint32_t>(
                    static_cast<unsigned char>(value[offset++]))
                << shift;
    }
    return number;
  }

  void recover_coordinator_database() {
    if (coordinator_database_.transaction_active() != leaves::tid_t(0)) {
      if (!coordinator_database_.commit(true)) {
        throw std::runtime_error(
            "could not recover the transaction monitor database");
      }
      coordinator_cursor_.update();
    }
  }

  void load() {
    coordinator_cursor_.find(leaves::Slice(transaction_id_));
    if (!coordinator_cursor_.is_valid()) {
      return;
    }

    const std::string value = coordinator_cursor_.value().string();
    if (value.size() < sizeof(kMagic) - 1 + 1 + sizeof(uint32_t) ||
        value.compare(0, sizeof(kMagic) - 1, kMagic) != 0) {
      throw std::runtime_error("invalid transaction monitor record");
    }

    size_t offset = sizeof(kMagic) - 1;
    const auto raw_state = static_cast<uint8_t>(value[offset++]);
    if (raw_state > static_cast<uint8_t>(State::committed)) {
      throw std::runtime_error("invalid transaction monitor state");
    }
    state_ = static_cast<State>(raw_state);

    const uint32_t count = read_u32(value, offset);
    if (count > 1024) {
      throw std::runtime_error("too many transaction monitor participants");
    }

    persisted_participants_.clear();
    persisted_participants_.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
      const uint32_t name_size = read_u32(value, offset);
      if (name_size > value.size() - offset ||
          value.size() - offset - name_size < sizeof(uint32_t) + 1) {
        throw std::runtime_error("truncated transaction monitor participant");
      }

      PersistedParticipant participant{
          value.substr(offset, name_size), leaves::tid_t(0), false};
      offset += name_size;
      participant.transaction_id = leaves::tid_t(read_u32(value, offset));
      participant.finished = value[offset++] != 0;
      persisted_participants_.push_back(std::move(participant));
    }
    if (offset != value.size()) {
      throw std::runtime_error("unexpected transaction monitor record data");
    }
  }

  void persist() {
    if (participants_.size() > 1024) {
      throw std::length_error("too many transaction monitor participants");
    }

    std::string value(kMagic, sizeof(kMagic) - 1);
    value.push_back(static_cast<char>(state_));
    append_u32(value, static_cast<uint32_t>(participants_.size()));
    for (const Participant& participant : participants_) {
      if (participant.name.size() >
          (std::numeric_limits<uint32_t>::max)()) {
        throw std::length_error("transaction participant name is too long");
      }
      append_u32(value, static_cast<uint32_t>(participant.name.size()));
      value.append(participant.name);
      append_u32(value, participant.transaction_id.value());
      value.push_back(participant.finished ? 1 : 0);
    }

    if (!coordinator_cursor_.start_transaction()) {
      throw std::runtime_error(
          "could not start monitor persistence transaction");
    }
    try {
      coordinator_cursor_.find(leaves::Slice(transaction_id_));
      coordinator_cursor_.value(leaves::Slice(value));
      if (!coordinator_cursor_.commit(true)) {
        throw std::runtime_error("could not persist transaction monitor state");
      }
    } catch (...) {
      if (coordinator_cursor_.is_transaction_active()) {
        coordinator_cursor_.rollback();
      }
      throw;
    }

    persisted_participants_.clear();
    for (const Participant& participant : participants_) {
      persisted_participants_.push_back(
          {participant.name, participant.transaction_id, participant.finished});
    }
    if (persist_hook_) {
      persist_hook_(state_);
    }
  }

  void validate_participants() const {
    if (participants_.empty()) {
      throw std::logic_error("transaction monitor has no participants");
    }
    if (state_ != State::none &&
        participants_.size() != persisted_participants_.size()) {
      throw std::logic_error(
          "transaction monitor recovery is missing participants");
    }
  }

  void begin_abort() {
    state_ = State::aborting;
    persist();
  }

  static void rollback_started(const std::vector<Cursor*>& started) noexcept {
    for (Cursor* cursor : started) {
      try {
        cursor->rollback();
      } catch (...) {
      }
    }
  }

  bool participant_transaction_is_recovered(
      const Participant& participant) const {
    const leaves::tid_t active = participant.database->transaction_active();
    if (active == leaves::tid_t(0)) {
      return false;
    }
    if (participant.transaction_id != leaves::tid_t(0) &&
        active != participant.transaction_id) {
      throw std::logic_error(
          "participant transaction id changed during recovery");
    }
    return true;
  }

  Progress finish_abort() {
    bool all_finished = true;
    for (Participant& participant : participants_) {
      if (participant.finished) {
        continue;
      }

      try {
        if (participant.cursor->is_transaction_active()) {
          participant.finished = participant.cursor->rollback();
        } else if (participant_transaction_is_recovered(participant)) {
          participant.finished = participant.database->rollback();
        } else {
          participant.finished = true;
        }
      } catch (...) {
        participant.finished = false;
      }

      if (participant.finished) {
        persist();
      }
      all_finished = all_finished && participant.finished;
    }

    if (!all_finished) {
      return Progress::pending;
    }
    state_ = State::aborted;
    persist();
    return Progress::complete;
  }

  Progress finish_commit() {
    bool all_finished = true;
    for (Participant& participant : participants_) {
      if (participant.finished) {
        continue;
      }

      try {
        if (participant.cursor->is_transaction_active()) {
          if (participant.cursor->txn_id() != participant.transaction_id) {
            throw std::logic_error("participant transaction id changed");
          }
          participant.finished = participant.cursor->commit(true);
        } else if (participant_transaction_is_recovered(participant)) {
          participant.finished = participant.database->commit(true);
        } else {
          participant.finished = true;
        }
      } catch (...) {
        participant.finished = false;
      }

      if (participant.finished) {
        persist();
      }
      all_finished = all_finished && participant.finished;
    }

    if (!all_finished) {
      return Progress::pending;
    }
    state_ = State::committed;
    persist();
    return Progress::complete;
  }

  Storage::storage_ptr storage_;
  Database coordinator_database_;
  Cursor coordinator_cursor_;
  std::string transaction_id_;
  PersistHook persist_hook_;
  State state_ = State::none;
  std::vector<Participant> participants_;
  std::vector<PersistedParticipant> persisted_participants_;
};

#endif