#pragma once

// this file defines the main b*+-tree index abstraction used by indexed
// columns.
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace dbms::index {

    struct IndexEntry {
        std::string key;
        std::vector<common::RowId> row_ids;
    };

    class BStarPlusTree {
      public:
        explicit BStarPlusTree(std::size_t max_keys_per_node = 8);

        void Insert(const std::string &key, common::RowId row_id);
        void Remove(const std::string &key, common::RowId row_id);
        [[nodiscard]] std::vector<common::RowId>
        Find(const std::string &key) const;
        [[nodiscard]] std::vector<IndexEntry>
        RangeScan(const std::string &lower_bound,
                  const std::string &upper_bound) const;
        [[nodiscard]] bool ValidateCanonicalInvariants() const;

      private:
        struct Node {
            explicit Node(bool leaf) : is_leaf(leaf) {}

            bool is_leaf{false};
            std::vector<std::string> keys;
            std::vector<std::vector<common::RowId>> values;
            std::vector<std::unique_ptr<Node>> children;
            Node *next_leaf{nullptr};
            Node *prev_leaf{nullptr};
        };

        struct Split {
            std::string separator_key;
            std::unique_ptr<Node> right;
        };

        [[nodiscard]] std::size_t MaxKeys() const;
        [[nodiscard]] std::size_t MinLeafKeys() const;
        [[nodiscard]] std::size_t MinInternalKeys() const;

        static std::size_t LowerBound(const std::vector<std::string> &keys,
                                      const std::string &key);
        static std::size_t UpperBound(const std::vector<std::string> &keys,
                                      const std::string &key);
        static bool ContainsRowId(const std::vector<common::RowId> &row_ids,
                                  common::RowId row_id);

        [[nodiscard]] Node *FindLeaf(const std::string &key) const;
        std::optional<Split> InsertRecursive(Node *node, const std::string &key,
                                             common::RowId row_id);
        Split SplitLeaf(Node *leaf);
        Split SplitInternal(Node *internal);
        bool TryRedistributeLeafWithLeft(Node *parent, std::size_t child_index);
        bool TryRedistributeLeafWithRight(Node *parent, std::size_t child_index);
        bool TryTwoToThreeSplitLeafWithRight(Node *parent,
                                             std::size_t child_index);
        bool TryTwoToThreeSplitLeafWithLeft(Node *parent,
                                            std::size_t child_index);
        bool TryRedistributeInternalWithLeft(Node *parent,
                                             std::size_t child_index);
        bool TryRedistributeInternalWithRight(Node *parent,
                                              std::size_t child_index);
        bool TryTwoToThreeSplitInternalWithRight(Node *parent,
                                                 std::size_t child_index);
        bool TryTwoToThreeSplitInternalWithLeft(Node *parent,
                                                std::size_t child_index);

        bool RemoveRecursive(Node *node, const std::string &key,
                             common::RowId row_id);
        void FixChildUnderflow(Node *parent, std::size_t child_index);
        void BorrowForLeaf(Node *parent, std::size_t left_index,
                           std::size_t right_index, bool borrow_from_left);
        void MergeLeaves(Node *parent, std::size_t left_index,
                         std::size_t right_index);
        void BorrowForInternal(Node *parent, std::size_t left_index,
                               std::size_t right_index, bool borrow_from_left);
        void MergeInternal(Node *parent, std::size_t left_index,
                           std::size_t right_index);
        bool TryThreeToTwoLeaf(Node *parent, std::size_t left_index);
        bool TryThreeToTwoInternal(Node *parent, std::size_t left_index);
        bool ValidateNode(const Node *node, bool is_root,
                          bool parent_is_root,
                          std::size_t expected_leaf_depth,
                          std::size_t current_depth,
                          const std::string *min_key,
                          const std::string *max_key,
                          const Node **previous_leaf) const;

        std::size_t max_keys_per_node_{8};
        std::unique_ptr<Node> root_;
    };

} // namespace dbms::index
