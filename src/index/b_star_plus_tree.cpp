#include "index/b_star_plus_tree.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace dbms::index {

    BStarPlusTree::BStarPlusTree(std::size_t max_keys_per_node)
        : max_keys_per_node_(std::max<std::size_t>(3, max_keys_per_node)),
          root_(std::make_unique<Node>(true)) {}

    std::size_t BStarPlusTree::MaxKeys() const { return max_keys_per_node_; }

    std::size_t BStarPlusTree::MinLeafKeys() const {
        return (MaxKeys() + 1) / 2;
    }

    std::size_t BStarPlusTree::MinInternalKeys() const {
        return (MaxKeys() + 1) / 2;
    }

    std::size_t
    BStarPlusTree::LowerBound(const std::vector<std::string> &keys,
                              const std::string &key) {
        return static_cast<std::size_t>(std::distance(
            keys.begin(), std::lower_bound(keys.begin(), keys.end(), key)));
    }

    std::size_t
    BStarPlusTree::UpperBound(const std::vector<std::string> &keys,
                              const std::string &key) {
        return static_cast<std::size_t>(std::distance(
            keys.begin(), std::upper_bound(keys.begin(), keys.end(), key)));
    }

    bool BStarPlusTree::ContainsRowId(const std::vector<common::RowId> &row_ids,
                                      common::RowId row_id) {
        return std::find(row_ids.begin(), row_ids.end(), row_id) != row_ids.end();
    }

    BStarPlusTree::Node *BStarPlusTree::FindLeaf(const std::string &key) const {
        Node *current = root_.get();
        while (!current->is_leaf) {
            const std::size_t child_index = UpperBound(current->keys, key);
            current = current->children[child_index].get();
        }
        return current;
    }

    BStarPlusTree::Split BStarPlusTree::SplitLeaf(Node *leaf) {
        auto right = std::make_unique<Node>(true);
        const std::size_t mid = leaf->keys.size() / 2;

        right->keys.assign(leaf->keys.begin() + static_cast<long>(mid),
                           leaf->keys.end());
        right->values.assign(leaf->values.begin() + static_cast<long>(mid),
                             leaf->values.end());

        leaf->keys.erase(leaf->keys.begin() + static_cast<long>(mid),
                         leaf->keys.end());
        leaf->values.erase(leaf->values.begin() + static_cast<long>(mid),
                           leaf->values.end());

        right->next_leaf = leaf->next_leaf;
        if (right->next_leaf != nullptr) {
            right->next_leaf->prev_leaf = right.get();
        }
        right->prev_leaf = leaf;
        leaf->next_leaf = right.get();

        return Split{.separator_key = right->keys.front(), .right = std::move(right)};
    }

    BStarPlusTree::Split BStarPlusTree::SplitInternal(Node *internal) {
        auto right = std::make_unique<Node>(false);
        const std::size_t mid = internal->keys.size() / 2;
        const std::string separator_key = internal->keys[mid];

        right->keys.assign(
            internal->keys.begin() + static_cast<long>(mid + 1),
            internal->keys.end());
        right->children.assign(
            std::make_move_iterator(
                internal->children.begin() + static_cast<long>(mid + 1)),
            std::make_move_iterator(internal->children.end()));

        internal->keys.erase(internal->keys.begin() + static_cast<long>(mid),
                             internal->keys.end());
        internal->children.erase(
            internal->children.begin() + static_cast<long>(mid + 1),
            internal->children.end());

        return Split{
            .separator_key = separator_key,
            .right = std::move(right),
        };
    }

    bool BStarPlusTree::TryRedistributeLeafWithLeft(Node *parent,
                                                    std::size_t child_index) {
        if (child_index == 0) {
            return false;
        }
        Node *leaf = parent->children[child_index].get();
        Node *left = parent->children[child_index - 1].get();
        if (!leaf->is_leaf || !left->is_leaf) {
            return false;
        }
        if (left->keys.size() >= MaxKeys() || leaf->keys.empty()) {
            return false;
        }

        left->keys.push_back(leaf->keys.front());
        left->values.push_back(leaf->values.front());
        leaf->keys.erase(leaf->keys.begin());
        leaf->values.erase(leaf->values.begin());
        parent->keys[child_index - 1] = leaf->keys.front();
        return true;
    }

    bool BStarPlusTree::TryRedistributeLeafWithRight(Node *parent,
                                                     std::size_t child_index) {
        if (child_index + 1 >= parent->children.size()) {
            return false;
        }
        Node *leaf = parent->children[child_index].get();
        Node *right = parent->children[child_index + 1].get();
        if (!leaf->is_leaf || !right->is_leaf) {
            return false;
        }
        if (right->keys.size() >= MaxKeys() || leaf->keys.empty()) {
            return false;
        }

        right->keys.insert(right->keys.begin(), leaf->keys.back());
        right->values.insert(right->values.begin(), leaf->values.back());
        leaf->keys.pop_back();
        leaf->values.pop_back();
        parent->keys[child_index] = right->keys.front();
        return true;
    }

    bool BStarPlusTree::TryTwoToThreeSplitLeafWithRight(Node *parent,
                                                         std::size_t child_index) {
        if (child_index + 1 >= parent->children.size() ||
            parent->keys.size() >= MaxKeys()) {
            return false;
        }
        Node *left_leaf = parent->children[child_index].get();
        Node *right_leaf = parent->children[child_index + 1].get();
        if (!left_leaf->is_leaf || !right_leaf->is_leaf) {
            return false;
        }
        if (left_leaf->keys.size() < MaxKeys() || right_leaf->keys.size() < MaxKeys()) {
            return false;
        }

        std::vector<std::string> all_keys;
        std::vector<std::vector<common::RowId>> all_values;
        all_keys.reserve(left_leaf->keys.size() + right_leaf->keys.size());
        all_values.reserve(left_leaf->values.size() + right_leaf->values.size());

        for (std::size_t index = 0; index < left_leaf->keys.size(); ++index) {
            all_keys.push_back(left_leaf->keys[index]);
            all_values.push_back(left_leaf->values[index]);
        }
        for (std::size_t index = 0; index < right_leaf->keys.size(); ++index) {
            all_keys.push_back(right_leaf->keys[index]);
            all_values.push_back(right_leaf->values[index]);
        }

        const std::size_t total = all_keys.size();
        const std::size_t first_size = total / 3;
        const std::size_t second_size = (total - first_size) / 2;
        const std::size_t third_size = total - first_size - second_size;
        if (first_size == 0 || second_size == 0 || third_size == 0) {
            return false;
        }

        auto middle_leaf = std::make_unique<Node>(true);

        left_leaf->keys.assign(all_keys.begin(),
                               all_keys.begin() + static_cast<long>(first_size));
        left_leaf->values.assign(all_values.begin(),
                                 all_values.begin() + static_cast<long>(first_size));

        middle_leaf->keys.assign(
            all_keys.begin() + static_cast<long>(first_size),
            all_keys.begin() + static_cast<long>(first_size + second_size));
        middle_leaf->values.assign(
            all_values.begin() + static_cast<long>(first_size),
            all_values.begin() + static_cast<long>(first_size + second_size));

        right_leaf->keys.assign(
            all_keys.begin() + static_cast<long>(first_size + second_size),
            all_keys.end());
        right_leaf->values.assign(
            all_values.begin() + static_cast<long>(first_size + second_size),
            all_values.end());

        middle_leaf->prev_leaf = left_leaf;
        middle_leaf->next_leaf = right_leaf;
        right_leaf->prev_leaf = middle_leaf.get();
        left_leaf->next_leaf = middle_leaf.get();

        parent->keys[child_index] = middle_leaf->keys.front();
        parent->keys.insert(parent->keys.begin() + static_cast<long>(child_index + 1),
                            right_leaf->keys.front());
        parent->children.insert(
            parent->children.begin() + static_cast<long>(child_index + 1),
            std::move(middle_leaf));
        return true;
    }

    bool BStarPlusTree::TryTwoToThreeSplitLeafWithLeft(Node *parent,
                                                        std::size_t child_index) {
        if (child_index == 0 || parent->keys.size() >= MaxKeys()) {
            return false;
        }
        return TryTwoToThreeSplitLeafWithRight(parent, child_index - 1);
    }

    bool BStarPlusTree::TryRedistributeInternalWithLeft(Node *parent,
                                                         std::size_t child_index) {
        if (child_index == 0) {
            return false;
        }
        Node *node = parent->children[child_index].get();
        Node *left = parent->children[child_index - 1].get();
        if (node->is_leaf || left->is_leaf) {
            return false;
        }
        if (left->keys.size() >= MaxKeys() || node->keys.empty() ||
            node->children.empty()) {
            return false;
        }

        left->keys.push_back(parent->keys[child_index - 1]);
        parent->keys[child_index - 1] = node->keys.front();
        node->keys.erase(node->keys.begin());

        left->children.push_back(std::move(node->children.front()));
        node->children.erase(node->children.begin());
        return true;
    }

    bool BStarPlusTree::TryRedistributeInternalWithRight(Node *parent,
                                                          std::size_t child_index) {
        if (child_index + 1 >= parent->children.size()) {
            return false;
        }
        Node *node = parent->children[child_index].get();
        Node *right = parent->children[child_index + 1].get();
        if (node->is_leaf || right->is_leaf) {
            return false;
        }
        if (right->keys.size() >= MaxKeys() || node->keys.empty() ||
            node->children.empty()) {
            return false;
        }

        right->keys.insert(right->keys.begin(), parent->keys[child_index]);
        parent->keys[child_index] = node->keys.back();
        node->keys.pop_back();

        right->children.insert(right->children.begin(),
                               std::move(node->children.back()));
        node->children.pop_back();
        return true;
    }

    bool BStarPlusTree::TryTwoToThreeSplitInternalWithRight(
        Node *parent, std::size_t child_index) {
        if (child_index + 1 >= parent->children.size() ||
            parent->keys.size() >= MaxKeys()) {
            return false;
        }
        Node *left_node = parent->children[child_index].get();
        Node *right_node = parent->children[child_index + 1].get();
        if (left_node->is_leaf || right_node->is_leaf) {
            return false;
        }
        if (left_node->keys.size() < MaxKeys() || right_node->keys.size() < MaxKeys()) {
            return false;
        }

        std::vector<std::string> all_keys;
        std::vector<std::unique_ptr<Node>> all_children;
        all_keys.reserve(left_node->keys.size() + right_node->keys.size() + 1);
        all_children.reserve(left_node->children.size() + right_node->children.size());

        for (const auto &key : left_node->keys) {
            all_keys.push_back(key);
        }
        all_keys.push_back(parent->keys[child_index]);
        for (const auto &key : right_node->keys) {
            all_keys.push_back(key);
        }

        for (auto &child : left_node->children) {
            all_children.push_back(std::move(child));
        }
        for (auto &child : right_node->children) {
            all_children.push_back(std::move(child));
        }

        const std::size_t total_keys = all_keys.size();
        const std::size_t left_keys_count = total_keys / 3;
        const std::size_t middle_keys_count = (total_keys - left_keys_count) / 2;
        const std::size_t right_keys_count =
            total_keys - left_keys_count - middle_keys_count - 2;
        if (left_keys_count == 0 || middle_keys_count == 0 || right_keys_count == 0) {
            return false;
        }

        const std::size_t sep1_index = left_keys_count;
        const std::size_t sep2_index = left_keys_count + 1 + middle_keys_count;
        const std::string sep1 = all_keys[sep1_index];
        const std::string sep2 = all_keys[sep2_index];

        auto middle_node = std::make_unique<Node>(false);

        left_node->keys.assign(all_keys.begin(),
                               all_keys.begin() + static_cast<long>(left_keys_count));
        middle_node->keys.assign(
            all_keys.begin() + static_cast<long>(sep1_index + 1),
            all_keys.begin() + static_cast<long>(sep2_index));
        right_node->keys.assign(all_keys.begin() + static_cast<long>(sep2_index + 1),
                                all_keys.end());

        left_node->children.clear();
        middle_node->children.clear();
        right_node->children.clear();

        const std::size_t left_children_count = left_keys_count + 1;
        const std::size_t middle_children_count = middle_keys_count + 1;
        const std::size_t right_children_count = right_keys_count + 1;

        std::size_t cursor = 0;
        for (std::size_t i = 0; i < left_children_count; ++i) {
            left_node->children.push_back(std::move(all_children[cursor++]));
        }
        for (std::size_t i = 0; i < middle_children_count; ++i) {
            middle_node->children.push_back(std::move(all_children[cursor++]));
        }
        for (std::size_t i = 0; i < right_children_count; ++i) {
            right_node->children.push_back(std::move(all_children[cursor++]));
        }

        parent->keys[child_index] = sep1;
        parent->keys.insert(parent->keys.begin() + static_cast<long>(child_index + 1),
                            sep2);
        parent->children.insert(
            parent->children.begin() + static_cast<long>(child_index + 1),
            std::move(middle_node));
        return true;
    }

    bool BStarPlusTree::TryTwoToThreeSplitInternalWithLeft(Node *parent,
                                                            std::size_t child_index) {
        if (child_index == 0 || parent->keys.size() >= MaxKeys()) {
            return false;
        }
        return TryTwoToThreeSplitInternalWithRight(parent, child_index - 1);
    }

    std::optional<BStarPlusTree::Split>
    BStarPlusTree::InsertRecursive(Node *node, const std::string &key,
                                   common::RowId row_id) {
        if (node->is_leaf) {
            const std::size_t position = LowerBound(node->keys, key);
            if (position < node->keys.size() && node->keys[position] == key) {
                auto &row_ids = node->values[position];
                if (!ContainsRowId(row_ids, row_id)) {
                    row_ids.push_back(row_id);
                }
                return std::nullopt;
            }

            node->keys.insert(node->keys.begin() + static_cast<long>(position), key);
            node->values.insert(
                node->values.begin() + static_cast<long>(position),
                std::vector<common::RowId>{row_id});

            if (node->keys.size() <= MaxKeys()) {
                return std::nullopt;
            }
            return SplitLeaf(node);
        }

        std::size_t child_index = UpperBound(node->keys, key);
        Node *child = node->children[child_index].get();
        if (child->keys.size() >= MaxKeys()) {
            bool adjusted = false;
            if (child->is_leaf) {
                adjusted = TryRedistributeLeafWithLeft(node, child_index) ||
                           TryRedistributeLeafWithRight(node, child_index) ||
                           TryTwoToThreeSplitLeafWithRight(node, child_index) ||
                           TryTwoToThreeSplitLeafWithLeft(node, child_index);
            } else {
                adjusted =
                    TryRedistributeInternalWithLeft(node, child_index) ||
                    TryRedistributeInternalWithRight(node, child_index) ||
                    TryTwoToThreeSplitInternalWithRight(node, child_index) ||
                    TryTwoToThreeSplitInternalWithLeft(node, child_index);
            }
            if (adjusted) {
                child_index = UpperBound(node->keys, key);
            }
        }

        auto child_split =
            InsertRecursive(node->children[child_index].get(), key, row_id);
        if (!child_split.has_value()) {
            return std::nullopt;
        }

        node->keys.insert(node->keys.begin() + static_cast<long>(child_index),
                          child_split->separator_key);
        node->children.insert(
            node->children.begin() + static_cast<long>(child_index + 1),
            std::move(child_split->right));

        if (node->keys.size() <= MaxKeys()) {
            return std::nullopt;
        }
        return SplitInternal(node);
    }

    void BStarPlusTree::Insert(const std::string &key, common::RowId row_id) {
        auto split = InsertRecursive(root_.get(), key, row_id);
        if (split.has_value()) {
            auto new_root = std::make_unique<Node>(false);
            new_root->keys.push_back(split->separator_key);
            new_root->children.push_back(std::move(root_));
            new_root->children.push_back(std::move(split->right));
            root_ = std::move(new_root);
        }
    }

    void BStarPlusTree::BorrowForLeaf(Node *parent, std::size_t left_index,
                                      std::size_t right_index,
                                      bool borrow_from_left) {
        Node *left = parent->children[left_index].get();
        Node *right = parent->children[right_index].get();
        if (borrow_from_left) {
            right->keys.insert(right->keys.begin(), left->keys.back());
            right->values.insert(right->values.begin(), left->values.back());
            left->keys.pop_back();
            left->values.pop_back();
            parent->keys[left_index] = right->keys.front();
            return;
        }

        left->keys.push_back(right->keys.front());
        left->values.push_back(right->values.front());
        right->keys.erase(right->keys.begin());
        right->values.erase(right->values.begin());
        parent->keys[left_index] = right->keys.front();
    }

    void BStarPlusTree::MergeLeaves(Node *parent, std::size_t left_index,
                                    std::size_t right_index) {
        Node *left = parent->children[left_index].get();
        Node *right = parent->children[right_index].get();

        left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
        left->values.insert(left->values.end(), right->values.begin(),
                            right->values.end());

        left->next_leaf = right->next_leaf;
        if (left->next_leaf != nullptr) {
            left->next_leaf->prev_leaf = left;
        }

        parent->keys.erase(parent->keys.begin() + static_cast<long>(left_index));
        parent->children.erase(parent->children.begin() +
                               static_cast<long>(right_index));
    }

    void BStarPlusTree::BorrowForInternal(Node *parent, std::size_t left_index,
                                          std::size_t right_index,
                                          bool borrow_from_left) {
        Node *left = parent->children[left_index].get();
        Node *right = parent->children[right_index].get();
        if (borrow_from_left) {
            right->keys.insert(right->keys.begin(), parent->keys[left_index]);
            parent->keys[left_index] = left->keys.back();
            left->keys.pop_back();

            right->children.insert(right->children.begin(),
                                   std::move(left->children.back()));
            left->children.pop_back();
            return;
        }

        left->keys.push_back(parent->keys[left_index]);
        parent->keys[left_index] = right->keys.front();
        right->keys.erase(right->keys.begin());

        left->children.push_back(std::move(right->children.front()));
        right->children.erase(right->children.begin());
    }

    void BStarPlusTree::MergeInternal(Node *parent, std::size_t left_index,
                                      std::size_t right_index) {
        Node *left = parent->children[left_index].get();
        Node *right = parent->children[right_index].get();

        left->keys.push_back(parent->keys[left_index]);
        left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
        for (auto &child : right->children) {
            left->children.push_back(std::move(child));
        }

        parent->keys.erase(parent->keys.begin() + static_cast<long>(left_index));
        parent->children.erase(parent->children.begin() +
                               static_cast<long>(right_index));
    }

    bool BStarPlusTree::TryThreeToTwoLeaf(Node *parent, std::size_t left_index) {
        if (left_index + 2 >= parent->children.size()) {
            return false;
        }
        Node *left = parent->children[left_index].get();
        Node *middle = parent->children[left_index + 1].get();
        Node *right = parent->children[left_index + 2].get();
        if (!left->is_leaf || !middle->is_leaf || !right->is_leaf) {
            return false;
        }

        std::vector<std::string> all_keys;
        std::vector<std::vector<common::RowId>> all_values;
        for (std::size_t i = 0; i < left->keys.size(); ++i) {
            all_keys.push_back(left->keys[i]);
            all_values.push_back(left->values[i]);
        }
        for (std::size_t i = 0; i < middle->keys.size(); ++i) {
            all_keys.push_back(middle->keys[i]);
            all_values.push_back(middle->values[i]);
        }
        for (std::size_t i = 0; i < right->keys.size(); ++i) {
            all_keys.push_back(right->keys[i]);
            all_values.push_back(right->values[i]);
        }
        if (all_keys.size() > 2 * MaxKeys()) {
            return false;
        }
        if (all_keys.size() < MinLeafKeys() * 2) {
            return false;
        }

        const std::size_t left_size = all_keys.size() / 2;
        const std::size_t right_size = all_keys.size() - left_size;
        if (left_size < MinLeafKeys() || right_size < MinLeafKeys()) {
            return false;
        }

        left->keys.assign(all_keys.begin(),
                          all_keys.begin() + static_cast<long>(left_size));
        left->values.assign(all_values.begin(),
                            all_values.begin() + static_cast<long>(left_size));
        middle->keys.assign(all_keys.begin() + static_cast<long>(left_size),
                            all_keys.end());
        middle->values.assign(all_values.begin() + static_cast<long>(left_size),
                              all_values.end());

        middle->next_leaf = right->next_leaf;
        if (middle->next_leaf != nullptr) {
            middle->next_leaf->prev_leaf = middle;
        }
        middle->prev_leaf = left;
        left->next_leaf = middle;

        parent->keys[left_index] = middle->keys.front();
        parent->keys.erase(parent->keys.begin() + static_cast<long>(left_index + 1));
        parent->children.erase(parent->children.begin() +
                               static_cast<long>(left_index + 2));
        return true;
    }

    bool BStarPlusTree::TryThreeToTwoInternal(Node *parent,
                                               std::size_t left_index) {
        if (left_index + 2 >= parent->children.size()) {
            return false;
        }
        Node *left = parent->children[left_index].get();
        Node *middle = parent->children[left_index + 1].get();
        Node *right = parent->children[left_index + 2].get();
        if (left->is_leaf || middle->is_leaf || right->is_leaf) {
            return false;
        }

        std::vector<std::string> all_keys;
        std::vector<std::unique_ptr<Node>> all_children;
        all_keys.reserve(left->keys.size() + middle->keys.size() + right->keys.size() +
                         2);
        all_children.reserve(left->children.size() + middle->children.size() +
                             right->children.size());

        for (const auto &key : left->keys) {
            all_keys.push_back(key);
        }
        all_keys.push_back(parent->keys[left_index]);
        for (const auto &key : middle->keys) {
            all_keys.push_back(key);
        }
        all_keys.push_back(parent->keys[left_index + 1]);
        for (const auto &key : right->keys) {
            all_keys.push_back(key);
        }

        for (auto &child : left->children) {
            all_children.push_back(std::move(child));
        }
        for (auto &child : middle->children) {
            all_children.push_back(std::move(child));
        }
        for (auto &child : right->children) {
            all_children.push_back(std::move(child));
        }
        if (all_keys.size() > (2 * MaxKeys() + 1)) {
            return false;
        }

        if (all_keys.size() < (MinInternalKeys() * 2 + 1)) {
            return false;
        }
        const std::size_t left_key_count = all_keys.size() / 2;
        const std::size_t sep_index = left_key_count;
        const std::size_t right_key_count = all_keys.size() - left_key_count - 1;
        if (left_key_count < MinInternalKeys() ||
            right_key_count < MinInternalKeys()) {
            return false;
        }
        const std::string new_separator = all_keys[sep_index];

        left->keys.assign(all_keys.begin(),
                          all_keys.begin() + static_cast<long>(left_key_count));
        middle->keys.assign(all_keys.begin() + static_cast<long>(sep_index + 1),
                            all_keys.end());

        left->children.clear();
        middle->children.clear();
        const std::size_t left_children_count = left_key_count + 1;
        const std::size_t right_children_count = right_key_count + 1;
        std::size_t cursor = 0;
        for (std::size_t i = 0; i < left_children_count; ++i) {
            left->children.push_back(std::move(all_children[cursor++]));
        }
        for (std::size_t i = 0; i < right_children_count; ++i) {
            middle->children.push_back(std::move(all_children[cursor++]));
        }

        parent->keys[left_index] = new_separator;
        parent->keys.erase(parent->keys.begin() + static_cast<long>(left_index + 1));
        parent->children.erase(parent->children.begin() +
                               static_cast<long>(left_index + 2));
        return true;
    }

    void BStarPlusTree::FixChildUnderflow(Node *parent, std::size_t child_index) {
        Node *child = parent->children[child_index].get();
        const std::size_t min_keys =
            child->is_leaf ? MinLeafKeys() : MinInternalKeys();
        if (child->keys.size() >= min_keys || parent->children.size() == 1) {
            return;
        }

        if (child_index > 0) {
            Node *left_sibling = parent->children[child_index - 1].get();
            if (left_sibling->keys.size() > min_keys) {
                if (child->is_leaf) {
                    BorrowForLeaf(parent, child_index - 1, child_index, true);
                } else {
                    BorrowForInternal(parent, child_index - 1, child_index, true);
                }
                return;
            }
        }

        if (child_index + 1 < parent->children.size()) {
            Node *right_sibling = parent->children[child_index + 1].get();
            if (right_sibling->keys.size() > min_keys) {
                if (child->is_leaf) {
                    BorrowForLeaf(parent, child_index, child_index + 1, false);
                } else {
                    BorrowForInternal(parent, child_index, child_index + 1, false);
                }
                return;
            }
        }

        if (parent->children.size() >= 3) {
            if (child_index > 0 && child_index + 1 < parent->children.size()) {
                const std::size_t left_index = child_index - 1;
                if (child->is_leaf) {
                    if (TryThreeToTwoLeaf(parent, left_index)) {
                        return;
                    }
                } else if (TryThreeToTwoInternal(parent, left_index)) {
                    return;
                }
            }
            if (child_index + 2 < parent->children.size()) {
                const std::size_t left_index = child_index;
                if (child->is_leaf) {
                    if (TryThreeToTwoLeaf(parent, left_index)) {
                        return;
                    }
                } else if (TryThreeToTwoInternal(parent, left_index)) {
                    return;
                }
            }
            if (child_index >= 2) {
                const std::size_t left_index = child_index - 2;
                if (child->is_leaf) {
                    if (TryThreeToTwoLeaf(parent, left_index)) {
                        return;
                    }
                } else if (TryThreeToTwoInternal(parent, left_index)) {
                    return;
                }
            }
        }

        if (parent->children.size() == 2) {
            if (child_index > 0) {
                if (child->is_leaf) {
                    MergeLeaves(parent, child_index - 1, child_index);
                } else {
                    MergeInternal(parent, child_index - 1, child_index);
                }
            } else if (child->is_leaf) {
                MergeLeaves(parent, child_index, child_index + 1);
            } else {
                MergeInternal(parent, child_index, child_index + 1);
            }
        }
    }

    bool BStarPlusTree::RemoveRecursive(Node *node, const std::string &key,
                                        common::RowId row_id) {
        if (node->is_leaf) {
            const std::size_t position = LowerBound(node->keys, key);
            if (position >= node->keys.size() || node->keys[position] != key) {
                return false;
            }
            auto &row_ids = node->values[position];
            auto row_id_iterator =
                std::find(row_ids.begin(), row_ids.end(), row_id);
            if (row_id_iterator == row_ids.end()) {
                return false;
            }
            row_ids.erase(row_id_iterator);
            if (row_ids.empty()) {
                node->keys.erase(node->keys.begin() + static_cast<long>(position));
                node->values.erase(node->values.begin() + static_cast<long>(position));
            }
            return true;
        }

        const std::size_t child_index = UpperBound(node->keys, key);
        const bool removed =
            RemoveRecursive(node->children[child_index].get(), key, row_id);
        if (!removed) {
            return false;
        }

        FixChildUnderflow(node, child_index);
        return true;
    }

    void BStarPlusTree::Remove(const std::string &key, common::RowId row_id) {
        (void)RemoveRecursive(root_.get(), key, row_id);

        if (!root_->is_leaf && root_->children.size() == 1) {
            root_ = std::move(root_->children.front());
        }
    }

    std::vector<common::RowId> BStarPlusTree::Find(const std::string &key) const {
        Node *leaf = FindLeaf(key);
        const std::size_t position = LowerBound(leaf->keys, key);
        if (position >= leaf->keys.size() || leaf->keys[position] != key) {
            return {};
        }
        return leaf->values[position];
    }

    std::vector<IndexEntry>
    BStarPlusTree::RangeScan(const std::string &lower_bound,
                             const std::string &upper_bound) const {
        std::vector<IndexEntry> entries;
        Node *leaf = FindLeaf(lower_bound);
        while (leaf != nullptr) {
            for (std::size_t index = 0; index < leaf->keys.size(); ++index) {
                const auto &key = leaf->keys[index];
                if (key < lower_bound) {
                    continue;
                }
                if (key >= upper_bound) {
                    return entries;
                }
                entries.push_back(IndexEntry{key, leaf->values[index]});
            }
            leaf = leaf->next_leaf;
        }
        return entries;
    }

    bool BStarPlusTree::ValidateNode(const Node *node, bool is_root,
                                     bool parent_is_root,
                                     std::size_t expected_leaf_depth,
                                     std::size_t current_depth,
                                     const std::string *min_key,
                                     const std::string *max_key,
                                     const Node **previous_leaf) const {
        if (node == nullptr) {
            return false;
        }
        if (node->keys.size() > MaxKeys()) {
            return false;
        }
        for (std::size_t index = 1; index < node->keys.size(); ++index) {
            if (node->keys[index - 1] >= node->keys[index]) {
                return false;
            }
        }

        if (!is_root && !parent_is_root) {
            const std::size_t min_keys =
                node->is_leaf ? MinLeafKeys() : MinInternalKeys();
            if (node->keys.size() < min_keys) {
                return false;
            }
        }

        if (node->is_leaf) {
            if (current_depth != expected_leaf_depth) {
                return false;
            }
            if (node->values.size() != node->keys.size()) {
                return false;
            }
            for (std::size_t index = 0; index < node->keys.size(); ++index) {
                const auto &key = node->keys[index];
                if (min_key != nullptr && key < *min_key) {
                    return false;
                }
                if (max_key != nullptr && key >= *max_key) {
                    return false;
                }
                if (node->values[index].empty()) {
                    return false;
                }
            }
            if (*previous_leaf != nullptr) {
                if ((*previous_leaf)->next_leaf != node || node->prev_leaf != *previous_leaf) {
                    return false;
                }
                if (!(*previous_leaf)->keys.empty() && !node->keys.empty() &&
                    (*previous_leaf)->keys.back() >= node->keys.front()) {
                    return false;
                }
            } else if (node->prev_leaf != nullptr) {
                return false;
            }
            *previous_leaf = node;
            return true;
        }

        if (node->children.size() != node->keys.size() + 1) {
            return false;
        }

        for (std::size_t index = 0; index < node->keys.size(); ++index) {
            const auto *left_max = &node->keys[index];
            const auto *right_min = &node->keys[index];

            if (!ValidateNode(node->children[index].get(), false, is_root,
                              expected_leaf_depth, current_depth + 1,
                              min_key, left_max, previous_leaf)) {
                return false;
            }
            min_key = right_min;
        }
        if (!ValidateNode(node->children.back().get(), false, is_root, expected_leaf_depth,
                          current_depth + 1, min_key, max_key,
                          previous_leaf)) {
            return false;
        }
        return true;
    }

    bool BStarPlusTree::ValidateCanonicalInvariants() const {
        if (root_ == nullptr) {
            return false;
        }
        std::size_t expected_leaf_depth = 0;
        const Node *cursor = root_.get();
        while (!cursor->is_leaf) {
            ++expected_leaf_depth;
            if (cursor->children.empty()) {
                return false;
            }
            cursor = cursor->children.front().get();
        }

        const Node *previous_leaf = nullptr;
        const bool ok = ValidateNode(root_.get(), true, false, expected_leaf_depth, 0,
                                     nullptr, nullptr, &previous_leaf);
        if (!ok) {
            return false;
        }
        if (previous_leaf != nullptr && previous_leaf->next_leaf != nullptr) {
            return false;
        }
        return true;
    }

} // namespace dbms::index
