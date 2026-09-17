#pragma once

#include <cassert>
#include <cstddef>

template <class Node>
class compact_stack {
    Node* head_ = nullptr;

public:
    constexpr compact_stack() noexcept = default;

    Node* front() const noexcept {
        return head_;
    }

    Node *push(Node& node) noexcept {
        auto old_head = head_;
        node.next = old_head;
        head_ = &node;
        return old_head;
    }

    Node* pop() noexcept {
        if (!head_) return nullptr;
        Node* item = head_;
        head_ = item->next;
        return item;
    }

    void unpush(Node *old_head) {
        assert(head_->next == old_head);
        head_ = old_head;
    }

    void clear() {
        head_ = nullptr;
    }
};

namespace {
    struct test_node {
        test_node* next;
    };
    static_assert(sizeof(compact_stack<test_node>) == sizeof(void*), 
                  "Queue head must be exactly 1 pointer");
}
