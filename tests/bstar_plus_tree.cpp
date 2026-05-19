#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "index/b_star_plus_tree.hpp"

int main() {
    dbms::index::BStarPlusTree tree(4);
    auto key_of = [](int value) {
        std::ostringstream output;
        output << std::setw(3) << std::setfill('0') << value;
        return output.str();
    };

    // Insert enough keys to force multiple leaf/internal splits.
    for (int key = 1; key <= 40; ++key) {
        tree.Insert(key_of(key), static_cast<dbms::common::RowId>(key));
    }

    // Point lookups.
    for (int key = 1; key <= 40; ++key) {
        const auto row_ids =
            tree.Find(key_of(key));
        assert(row_ids.size() == 1);
        assert(row_ids[0] == static_cast<dbms::common::RowId>(key));
    }
    assert(tree.Find("404").empty());

    // Duplicates for one key must stay attached to the same key.
    tree.Insert("010", 1000);
    tree.Insert("010", 1001);
    auto key_ten_rows = tree.Find("010");
    std::sort(key_ten_rows.begin(), key_ten_rows.end());
    assert(key_ten_rows.size() == 3);
    assert(key_ten_rows[0] == 10);
    assert(key_ten_rows[1] == 1000);
    assert(key_ten_rows[2] == 1001);

    // Range scan must preserve order and bounds [lower, upper).
    const auto range_entries = tree.RangeScan("015", "020");
    assert(!range_entries.empty());
    assert(range_entries.front().key == "015");
    assert(range_entries.back().key == "019");
    for (std::size_t index = 1; index < range_entries.size(); ++index) {
        assert(range_entries[index - 1].key < range_entries[index].key);
    }

    // Remove duplicates and then base row id.
    tree.Remove("010", 1000);
    tree.Remove("010", 1001);
    auto rows_after_duplicate_remove = tree.Find("010");
    assert(rows_after_duplicate_remove.size() == 1);
    assert(rows_after_duplicate_remove[0] == 10);
    tree.Remove("010", 10);
    assert(tree.Find("010").empty());

    // Remove many keys to trigger merges/redistribution.
    for (int key = 1; key <= 35; ++key) {
        tree.Remove(key_of(key), static_cast<dbms::common::RowId>(key));
    }
    for (int key = 1; key <= 35; ++key) {
        assert(tree.Find(key_of(key)).empty());
    }
    for (int key = 36; key <= 40; ++key) {
        auto row_ids = tree.Find(key_of(key));
        assert(row_ids.size() == 1);
        assert(row_ids[0] == static_cast<dbms::common::RowId>(key));
    }

    // Remove all keys: tree must stay operational.
    for (int key = 36; key <= 40; ++key) {
        tree.Remove(key_of(key), static_cast<dbms::common::RowId>(key));
    }
    assert(tree.RangeScan("0", "999").empty());
    tree.Insert("500", 500);
    auto final_rows = tree.Find("500");
    assert(final_rows.size() == 1);
    assert(final_rows[0] == 500);

    return 0;
}
