#include <algorithm>
#include <cassert>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "index/b_star_plus_tree.hpp"

namespace {

    std::string KeyOf(int value) {
        std::ostringstream output;
        output << std::setw(5) << std::setfill('0') << value;
        return output.str();
    }

    std::vector<dbms::common::RowId> NormalizeRowIds(
        std::vector<dbms::common::RowId> row_ids) {
        std::sort(row_ids.begin(), row_ids.end());
        return row_ids;
    }

} // namespace

int main() {
    dbms::index::BStarPlusTree tree(9);
    std::map<std::string, std::vector<dbms::common::RowId>> reference;

    std::mt19937_64 rng(0xB57A11ULL);
    std::uniform_int_distribution<int> key_distribution(0, 400);
    std::uniform_int_distribution<int> op_distribution(0, 99);
    std::uniform_int_distribution<int> row_distribution(0, 1000000);

    constexpr int kOperations = 20000;
    for (int step = 0; step < kOperations; ++step) {
        const int operation = op_distribution(rng);

        if (operation < 55) {
            // Insert.
            const std::string key = KeyOf(key_distribution(rng));
            const dbms::common::RowId row_id =
                static_cast<dbms::common::RowId>(row_distribution(rng));
            tree.Insert(key, row_id);
            auto &row_ids = reference[key];
            if (std::find(row_ids.begin(), row_ids.end(), row_id) == row_ids.end()) {
                row_ids.push_back(row_id);
            }
        } else if (operation < 80) {
            // Remove if key exists in reference.
            if (!reference.empty()) {
                auto iterator = reference.begin();
                std::advance(iterator,
                             static_cast<long>(rng() % reference.size()));
                const std::string key = iterator->first;
                auto &row_ids = iterator->second;
                const std::size_t index =
                    static_cast<std::size_t>(rng() % row_ids.size());
                const dbms::common::RowId row_id = row_ids[index];
                tree.Remove(key, row_id);
                row_ids.erase(row_ids.begin() + static_cast<long>(index));
                if (row_ids.empty()) {
                    reference.erase(key);
                }
            }
        } else if (operation < 92) {
            // Point lookup check.
            const std::string key = KeyOf(key_distribution(rng));
            const auto expected_iterator = reference.find(key);
            const auto actual_row_ids = NormalizeRowIds(tree.Find(key));
            if (expected_iterator == reference.end()) {
                assert(actual_row_ids.empty());
            } else {
                auto expected_row_ids =
                    NormalizeRowIds(expected_iterator->second);
                assert(actual_row_ids == expected_row_ids);
            }
        } else {
            // Range lookup check.
            int left = key_distribution(rng);
            int right = key_distribution(rng);
            if (left > right) {
                std::swap(left, right);
            }
            const std::string lower = KeyOf(left);
            const std::string upper = KeyOf(right + 1);

            const auto actual_entries = tree.RangeScan(lower, upper);
            std::vector<std::pair<std::string, std::vector<dbms::common::RowId>>>
                expected_entries;
            for (auto iterator = reference.lower_bound(lower);
                 iterator != reference.end() && iterator->first < upper;
                 ++iterator) {
                expected_entries.push_back(
                    {iterator->first, NormalizeRowIds(iterator->second)});
            }

            assert(actual_entries.size() == expected_entries.size());
            for (std::size_t index = 0; index < actual_entries.size(); ++index) {
                assert(actual_entries[index].key == expected_entries[index].first);
                assert(NormalizeRowIds(actual_entries[index].row_ids) ==
                       expected_entries[index].second);
            }
        }

        assert(tree.ValidateCanonicalInvariants());
    }

    // Final full comparison.
    const auto all_entries = tree.RangeScan("00000", "99999");
    assert(all_entries.size() == reference.size());
    std::size_t index = 0;
    for (const auto &entry : reference) {
        assert(all_entries[index].key == entry.first);
        assert(NormalizeRowIds(all_entries[index].row_ids) ==
               NormalizeRowIds(entry.second));
        ++index;
    }

    return 0;
}
