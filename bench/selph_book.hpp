#pragma once
// Reference implementation of the design in WK Selph, "How to Build a Fast
// Limit Order Book" (2011), for benchmarking only.
//
//   - Limit objects (one per price) in a balanced binary tree (std::map = red-black tree)
//   - Each Limit also in a hash map keyed by price -> O(1) lookup for existing levels
//   - Each Order heap-allocated, in an intrusive doubly linked list on its Limit
//   - Each Order also in a hash map keyed by id -> O(1) cancel
//   - Cached best bid / best ask pointers

#include "order_book.hpp" // for Side

#include <cstdint>
#include <map>
#include <unordered_map>

namespace selph {

struct Limit;

struct Order {
    uint64_t id;
    uint32_t shares;
    Side     side;
    Order*   next = nullptr;
    Order*   prev = nullptr;
    Limit*   parent_limit = nullptr;
};

struct Limit {
    int64_t  price;
    uint32_t size = 0;          // number of orders
    uint64_t total_volume = 0;
    Order*   head = nullptr;
    Order*   tail = nullptr;
};

class Book {
public:
    Book() = default;
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;
    ~Book() {
        for (auto& [id, o] : orders_) delete o;
    }

    void reserve(size_t orders, size_t levels) {
        orders_.reserve(orders);
        bid_by_price_.reserve(levels);
        ask_by_price_.reserve(levels);
    }

    void add(uint64_t id, Side side, int64_t price, uint32_t shares) {
        auto& by_price = side == Side::Buy ? bid_by_price_ : ask_by_price_;
        auto& tree     = side == Side::Buy ? bid_tree_     : ask_tree_;

        Limit* lim;
        auto it = by_price.find(price);
        if (it != by_price.end()) {
            lim = it->second;                       // existing level: O(1)
        } else {
            lim = &tree.try_emplace(price, Limit{price}).first->second; // new level: O(log M)
            by_price.emplace(price, lim);
            if (side == Side::Buy) { if (!best_bid_ || price > best_bid_->price) best_bid_ = lim; }
            else                   { if (!best_ask_ || price < best_ask_->price) best_ask_ = lim; }
        }

        Order* o = new Order{id, shares, side, nullptr, lim->tail, lim};
        if (lim->tail) lim->tail->next = o; else lim->head = o;
        lim->tail = o;
        lim->size += 1;
        lim->total_volume += shares;

        orders_.emplace(id, o);
    }

    size_t num_orders() const { return orders_.size(); }
    const Limit* best_bid() const { return best_bid_; }

private:
    std::map<int64_t, Limit> bid_tree_, ask_tree_;
    std::unordered_map<int64_t, Limit*> bid_by_price_, ask_by_price_;
    std::unordered_map<uint64_t, Order*> orders_;
    Limit* best_bid_ = nullptr;
    Limit* best_ask_ = nullptr;
};

} // namespace selph
