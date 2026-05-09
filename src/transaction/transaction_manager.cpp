#include "transaction/transaction_manager.hpp"

// this file will handle begin, commit, rollback, and concurrency hooks.
namespace dbms::transaction {

TransactionId TransactionManager::Begin() {
    return next_id_++;
}

void TransactionManager::Commit(TransactionId) {}

void TransactionManager::Rollback(TransactionId) {}

}  // namespace dbms::transaction
