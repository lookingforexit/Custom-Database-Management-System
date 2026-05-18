#include "transaction/transaction_manager.hpp"

// this file implements basic transaction lifecycle behavior.
namespace dbms::transaction {

  TransactionId TransactionManager::Begin() { return next_id_++; }

  void TransactionManager::Commit(TransactionId) {}

  void TransactionManager::Rollback(TransactionId) {}

} // namespace dbms::transaction
