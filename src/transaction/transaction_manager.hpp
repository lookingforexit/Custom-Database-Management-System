#pragma once

// this file defines basic transaction lifecycle management interfaces.
#include <cstdint>

namespace dbms::transaction {

    using TransactionId = std::uint64_t;

    class TransactionManager {
      public:
        [[nodiscard]] TransactionId Begin();
        void Commit(TransactionId transaction_id);
        void Rollback(TransactionId transaction_id);

      private:
        TransactionId next_id_{1};
    };

} // namespace dbms::transaction
